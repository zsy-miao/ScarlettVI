#include "title_modules.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace scarlett {
namespace {
std::string lower(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return name;
}
std::string symbol_description(const ImportedSymbol& symbol) {
    return symbol.by_ordinal ? "#" + std::to_string(symbol.ordinal) : symbol.name;
}
std::string normalize_dll(std::string name) {
    name = lower(std::move(name));
    if (name.find_first_of("/\\:") != std::string::npos)
        throw std::runtime_error("Invalid DLL dependency name");
    if (!name.ends_with(".dll"))
        name += ".dll";
    return name;
}
// PE forwarder string grammar: targetdll.ExportName or targetdll.#Ordinal.
// Split at the LAST '.', to accept a redundant '.dll' suffix.
bool forward_target(const std::string& text, std::string& dll, ImportedSymbol& symbol) {
    const size_t pos = text.rfind('.');
    if (pos == std::string::npos || pos == 0 || pos + 1 == text.size())
        return false;
    dll = normalize_dll(text.substr(0, pos));
    const std::string raw = text.substr(pos + 1);
    if (raw[0] == '#') {
        unsigned ordinal = 0;
        const auto [last, ec] = std::from_chars(raw.data() + 1, raw.data() + raw.size(), ordinal);
        if (ec != std::errc{} || last != raw.data() + raw.size() || ordinal > 65535)
            return false;
        symbol.by_ordinal = true;
        symbol.ordinal = static_cast<uint16_t>(ordinal);
    } else {
        symbol.name = raw;
    }
    return true;
}
}

TitleModules::TitleModules(std::filesystem::path title_directory, const PeFile& executable)
    : directory_(std::move(title_directory)) {
    find_dependencies(executable);
}

void TitleModules::find_dependencies(const PeFile& pe) {
    auto require = [&](const std::string& original) {
        const std::string key = normalize_dll(original);
        if (classify_import(key, directory_) != ImportRoute::TitleLocal || modules_.contains(key))
            return;
        const std::filesystem::path file = directory_ / key;
        auto record = std::make_unique<Record>(file);
        const PeFile* recurse = &record->pe;
        modules_.emplace(key, std::move(record)); // before recursion: permits cyclic imports
        find_dependencies(*recurse);
    };
    for (const auto& imp : pe.details().imports)
        require(imp.name);
    // Export forwarders can reference local modules absent from direct imports.
    for (const auto& exp : pe.details().exports) {
        if (exp.forwarder.empty()) continue;
        std::string dll;
        ImportedSymbol target;
        if (forward_target(exp.forwarder, dll, target))
            require(dll);
    }
}

uint64_t TitleModules::resolve_export(const std::string& module_name,
                                      const ImportedSymbol& imported,
                                      ImportRouter& router,
                                      std::vector<std::string>& visited) const {
    if (visited.size() >= 32) return 0;
    const auto key = normalize_dll(module_name);
    const auto item = key + "!" + lower(symbol_description(imported));
    if (std::find(visited.begin(), visited.end(), item) != visited.end())
        return 0; // cyclic forwarders
    visited.push_back(item);

    if (classify_import(key, directory_) == ImportRoute::Windows) {
        FARPROC proc = router.resolve_system_export(key, imported);
        visited.pop_back();
        return reinterpret_cast<uint64_t>(proc);
    }
    const auto module = modules_.find(key);
    if (module == modules_.end()) {
        visited.pop_back();
        return 0;
    }
    const auto& exports = module->second->pe.details().exports;
    const auto found = std::find_if(exports.begin(), exports.end(), [&](const ExportedSymbol& exp) {
        return imported.by_ordinal ? exp.ordinal == imported.ordinal
                                   : !exp.name.empty() && exp.name == imported.name;
    });
    if (found == exports.end() || found->rva == 0) {
        visited.pop_back();
        return 0;
    }
    uint64_t address = 0;
    if (found->forwarder.empty()) {
        if (found->rva < module->second->pe.details().image_size)
            address = module->second->image.base_address() + found->rva;
    } else {
        std::string dll;
        ImportedSymbol forwarded;
        if (forward_target(found->forwarder, dll, forwarded))
            address = resolve_export(dll, forwarded, router, visited);
    }
    visited.pop_back();
    return address;
}

void TitleModules::bind_local(const PeFile& pe, MappedImage& image,
                              ImportRouter& router, TitleBindingReport& report) {
    for (const auto& module : pe.details().imports) {
        if (classify_import(module.name, directory_) != ImportRoute::TitleLocal)
            continue;
        for (const auto& imp : module.symbols) {
            ++report.local_imports_seen;
            std::vector<std::string> visited;
            const uint64_t address = resolve_export(module.name, imp, router, visited);
            if (address) {
                image.patch_iat(imp.iat_rva, address);
                ++report.local_imports_bound;
            } else {
                report.missing_local_exports.push_back(module.name + "!" + symbol_description(imp));
                report.pending.push_back(PendingImport{
                    pe.path().filename().string(),
                    module.name,
                    symbol_description(imp),
                    imp.iat_rva,
                    PendingKind::TitleExport,
                    module.delay_loaded
                });
            }
        }
    }
}

TitleBindingReport TitleModules::bind(const PeFile& executable, MappedImage& executable_image,
                                      ImportRouter& router) {
    TitleBindingReport report;
    report.title_images_mapped = modules_.size();
    bind_local(executable, executable_image, router, report);
    for (auto& [name, module] : modules_) {
        bind_local(module->pe, module->image, router, report);
        const BindingResult binding = router.bind_windows(module->pe, module->image, directory_);
        report.nested_host_seen += binding.host_symbols_seen;
        report.nested_host_bound += binding.host_symbols_bound;
        report.nested_release_hooks_bound += binding.memory_release_hooks_bound;
        report.nested_xbox_pending += binding.xbox_symbols_pending;
        report.nested_experimental_hle_bound += binding.experimental_hle_symbols_bound;
        report.nested_apu_host_alloc_bound += binding.experimental_apu_host_alloc_bound;
        report.nested_apu_hal_partial_bound += binding.experimental_apu_hal_partial_bound;
        report.nested_chad_client_bound += binding.experimental_chad_client_bound;
        report.nested_chad_host_fir_bound += binding.experimental_chad_host_fir_bound;
        report.nested_chad_error_only_bound += binding.experimental_chad_error_only_bound;
        report.nested_xapu_partial_bound += binding.experimental_xapu_partial_bound;
        report.nested_xapu_host_fir_bound += binding.experimental_xapu_host_fir_bound;
        report.nested_dstorage_factory_partial_bound += binding.experimental_dstorage_factory_partial_bound;
        report.nested_xmem_error_only_bound += binding.experimental_xmem_error_only_bound;
        report.nested_xmem_budget_partial_bound += binding.experimental_xmem_budget_partial_bound;
        report.nested_d3d12_exports_bound += binding.experimental_d3d12_exports_bound;
        report.nested_d3d12_device_bound += binding.experimental_d3d12_device_bound;
        report.nested_gpu_va_config_bound += binding.experimental_gpu_va_config_bound;
        report.nested_xg_error_only_bound += binding.experimental_xg_error_only_bound;
        report.nested_unknown_pending += binding.unknown_symbols_pending;
        report.missing_nested_host.insert(report.missing_nested_host.end(),
            binding.missing_host_symbols.begin(), binding.missing_host_symbols.end());
        report.pending.insert(report.pending.end(), binding.pending.begin(), binding.pending.end());
        const ImageFinalizeReport finalized = module->image.finalize_image();
        report.protected_sections += finalized.sections_protected;
        report.registered_unwind_functions += finalized.unwind_functions_registered;
        report.tls_slots_prepared += finalized.tls_slots_prepared;
        report.tls_template_bytes += finalized.tls_template_bytes;
        report.deferred_tls_callbacks += finalized.deferred_tls_callbacks;
    }
    return report;
}
} // namespace scarlett
