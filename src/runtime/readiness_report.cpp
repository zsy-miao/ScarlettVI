#include "readiness_report.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace scarlett {
namespace {
std::string escape_json(std::string_view input) {
    std::string result;
    result.reserve(input.size() + 8);
    const char* hex = "0123456789abcdef";
    for (unsigned char c : input) {
        if (c == '"') result += "\\\"";
        else if (c == '\\') result += "\\\\";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else if (c < 0x20) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 0x0f];
        } else result += static_cast<char>(c);
    }
    return result;
}
size_t kind_count(const StartupReadiness& report, PendingKind kind) {
    return static_cast<size_t>(std::count_if(report.pending.begin(), report.pending.end(),
        [kind](const PendingImport& entry) { return entry.kind == kind; }));
}
}

StartupReadiness assess_startup_readiness(const PeFile& executable,
    const BindingResult& primary, const TitleBindingReport& nested,
    const ImageFinalizeReport& primary_finalize) {
    StartupReadiness report;
    report.executable = executable.path().filename().string();
    report.local_dlls = nested.title_images_mapped;
    report.windows_imports_bound = primary.host_symbols_bound + nested.nested_host_bound;
    report.windows_imports_total = primary.host_symbols_seen + nested.nested_host_seen;
    report.title_imports_bound = nested.local_imports_bound;
    report.title_imports_total = nested.local_imports_seen;
    report.xbox_experimental_memory_bindings =
        primary.experimental_hle_symbols_bound + nested.nested_experimental_hle_bound;
    report.xbox_xmem_error_only_bindings =
        primary.experimental_xmem_error_only_bound +
        nested.nested_xmem_error_only_bound;
    report.xbox_apu_host_alloc_bindings =
        primary.experimental_apu_host_alloc_bound +
        nested.nested_apu_host_alloc_bound;
    report.xbox_experimental_d3d12_bindings =
        primary.experimental_d3d12_exports_bound + nested.nested_d3d12_exports_bound;
    report.xbox_experimental_device11_bindings =
        primary.experimental_d3d12_device_bound + nested.nested_d3d12_device_bound;
    report.xbox_experimental_gpu_va_config_bindings =
        primary.experimental_gpu_va_config_bound + nested.nested_gpu_va_config_bound;
    report.registered_unwind_functions =
        primary_finalize.unwind_functions_registered + nested.registered_unwind_functions;
    report.tls_templates_prepared =
        primary_finalize.tls_slots_prepared + nested.tls_slots_prepared;
    report.tls_callbacks_deferred =
        primary_finalize.deferred_tls_callbacks + nested.deferred_tls_callbacks;
    report.pending = primary.pending;
    report.pending.insert(report.pending.end(), nested.pending.begin(), nested.pending.end());
    std::sort(report.pending.begin(), report.pending.end(), [](const auto& a, const auto& b) {
        return std::tie(a.module, a.owner, a.symbol, a.iat_rva) <
               std::tie(b.module, b.owner, b.symbol, b.iat_rva);
    });

    // Verify the structured diagnostics are complete: no pending references
    // should be silently dropped while producing a startup report.
    const auto xbox = kind_count(report, PendingKind::XboxHle);
    const auto unknown = kind_count(report, PendingKind::UnknownModule);
    const auto host = kind_count(report, PendingKind::WindowsExport);
    const auto local = kind_count(report, PendingKind::TitleExport);
    if (xbox != primary.xbox_symbols_pending + nested.nested_xbox_pending ||
        unknown != primary.unknown_symbols_pending + nested.nested_unknown_pending ||
        host != primary.missing_host_symbols.size() + nested.missing_nested_host.size() ||
        local != nested.missing_local_exports.size())
        throw std::runtime_error("Import blockers report does not match binding counts");

    report.required_runtime_milestones = {
        "Xbox D3D12_XS and XG device, GPU memory, resource and command ABI translation",
        "Xbox RDNA2 shader and texture-layout translation to compatible host resources",
        "Xbox audio/DSP, DirectStorage and remaining system-service high-level emulation",
        "Native title DLL process initialization and TLS callback sequencing",
        "Static TLS lifecycle across additional title-created threads",
        "Console process environment, filesystem and title startup fault isolation"
    };
    // Avoid a false-ready signal when all imported functions have addresses
    // but those functions or process initializers are not yet usable.
    report.safe_to_execute_title = report.pending.empty() &&
                                    report.required_runtime_milestones.empty();
    return report;
}

void print_startup_readiness(const StartupReadiness& report, std::ostream& output) {
    output << "\nScarlettVI | native title startup-readiness assessment\n"
           << "Executable: " << report.executable << "\n"
           << "Windows import slots: " << report.windows_imports_bound << "/"
           << report.windows_imports_total << "\n"
           << "Title-local import slots: " << report.title_imports_bound << "/"
           << report.title_imports_total << " across " << report.local_dlls << " DLLs\n"
           << "Experimental Xbox links: XMem=" << report.xbox_experimental_memory_bindings
           << " (error-only=" << report.xbox_xmem_error_only_bindings << ")"
           << ", APU host memory=" << report.xbox_apu_host_alloc_bindings
           << ", D3D12 serializer=" << report.xbox_experimental_d3d12_bindings
           << ", host Device11 factory=" << report.xbox_experimental_device11_bindings
           << ", sparse GPU VA policy=" << report.xbox_experimental_gpu_va_config_bindings
           << " (Xbox GPU tile mappings/flags are NOT implemented)\n"
           << "Remaining imports: " << report.pending.size()
           << " (Xbox=" << kind_count(report, PendingKind::XboxHle)
           << ", unknown=" << kind_count(report, PendingKind::UnknownModule)
           << ", Windows=" << kind_count(report, PendingKind::WindowsExport)
           << ", title-local=" << kind_count(report, PendingKind::TitleExport) << ")\n";

    std::map<std::string, std::vector<const PendingImport*>> grouped;
    for (const auto& entry : report.pending)
        grouped[entry.module].push_back(&entry);
    if (!grouped.empty()) output << "\nUnresolved import references grouped by dependency:\n";
    for (const auto& [module, imports] : grouped) {
        output << "  " << module << " (" << imports.size() << ")\n";
        for (const auto* entry : imports) {
            output << "    [" << pending_kind_name(entry->kind) << "] "
                   << entry->owner << " -> " << entry->symbol
                   << " @IAT 0x" << std::hex << entry->iat_rva << std::dec
                   << (entry->delay_loaded ? " [delay]" : "") << "\n";
        }
    }

    output << "\nOutstanding runtime milestones independent of import addresses:\n";
    for (const auto& milestone : report.required_runtime_milestones)
        output << "  - " << milestone << "\n";
    output << "\nTITLE EXECUTION: "
           << (report.safe_to_execute_title ? "READY" : "BLOCKED")
           << " | diagnostic mode never executes title code\n";
}

void write_startup_readiness_json(const StartupReadiness& report,
                                  const std::filesystem::path& output_path) {
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path());
    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot open startup-readiness JSON output");
    out << "{\n  \"schema\": \"scarlettvi-readiness-v1\",\n"
        << "  \"executable\": \"" << escape_json(report.executable) << "\",\n"
        << "  \"title_execution_ready\": " << (report.safe_to_execute_title ? "true" : "false") << ",\n"
        << "  \"title_code_executed\": false,\n"
        << "  \"summary\": {\n"
        << "    \"mapped_title_dlls\": " << report.local_dlls << ",\n"
        << "    \"windows_bound\": " << report.windows_imports_bound << ",\n"
        << "    \"windows_total\": " << report.windows_imports_total << ",\n"
        << "    \"title_bound\": " << report.title_imports_bound << ",\n"
        << "    \"title_total\": " << report.title_imports_total << ",\n"
        << "    \"experimental_xmem_bound\": " << report.xbox_experimental_memory_bindings << ",\n"
        << "    \"experimental_xmem_error_only_bound\": " << report.xbox_xmem_error_only_bindings << ",\n"
        << "    \"experimental_apu_host_alloc_bound\": " << report.xbox_apu_host_alloc_bindings << ",\n"
        << "    \"experimental_d3d12_bound\": " << report.xbox_experimental_d3d12_bindings << ",\n"
        << "    \"experimental_device11_factory_bound\": " << report.xbox_experimental_device11_bindings << ",\n"
        << "    \"experimental_gpu_va_config_bound\": " << report.xbox_experimental_gpu_va_config_bindings << ",\n"
        << "    \"unresolved_references\": " << report.pending.size() << ",\n"
        << "    \"unresolved_xbox\": " << kind_count(report, PendingKind::XboxHle) << ",\n"
        << "    \"registered_unwind_functions\": " << report.registered_unwind_functions << ",\n"
        << "    \"current_thread_tls_templates\": " << report.tls_templates_prepared << ",\n"
        << "    \"deferred_tls_callbacks\": " << report.tls_callbacks_deferred << "\n"
        << "  },\n  \"pending_imports\": [\n";
    for (size_t i = 0; i < report.pending.size(); ++i) {
        const auto& p = report.pending[i];
        out << "    {\"owner\": \"" << escape_json(p.owner)
            << "\", \"module\": \"" << escape_json(p.module)
            << "\", \"symbol\": \"" << escape_json(p.symbol)
            << "\", \"kind\": \"" << pending_kind_name(p.kind)
            << "\", \"iat_rva\": " << p.iat_rva
            << ", \"delay_loaded\": " << (p.delay_loaded ? "true" : "false")
            << "}" << (i + 1 < report.pending.size() ? "," : "") << "\n";
    }
    out << "  ],\n  \"required_runtime_milestones\": [\n";
    for (size_t i = 0; i < report.required_runtime_milestones.size(); ++i)
        out << "    \"" << escape_json(report.required_runtime_milestones[i])
            << "\"" << (i + 1 < report.required_runtime_milestones.size() ? "," : "") << "\n";
    out << "  ]\n}\n";
    if (!out) throw std::runtime_error("Could not write complete readiness report");
}
} // namespace scarlett
