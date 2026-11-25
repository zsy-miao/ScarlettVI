#include "pe/pe_file.h"
#include "analysis/xg_callsite_probe.h"
#include "analysis/xg_field_projection.h"
#include "analysis/xg_vcall_candidates.h"
#include "analysis/xg_vcall_loads.h"
#include "analysis/xg_vcall_arguments.h"
#include "analysis/xg_vcall_stack_writes.h"
#include "analysis/xg_vcall_registers.h"
#include "analysis/xg_vcall_successors.h"
#include "analysis/xg_vcall_indexed_reads.h"
#include "loader/image_mapper.h"
#include "loader/synthetic_entry_test.h"
#include "runtime/import_router.h"
#include "runtime/readiness_report.h"
#include "loader/title_modules.h"
#include "formats/rpf9_header.h"
#include "gpu/host_backend.h"
#include "gpu/device_context.h"
#include "gpu/guest_address_space.h"
#include "gpu/xbox_device_factory.h"
#include "gpu/sparse_gpu_windows.h"
#include "gpu/texture_layout.h"
#include <d3d12sdklayers.h>
#include "runtime/physical_memory.h"
#include "runtime/xmem_bridge.h"
#include "audio/apu_memory.h"
#include "audio/apu_shape.h"
#include "audio/acp_hal_host.h"
#include "audio/xapu_host.h"
#include "audio/dsp_convolution.h"
#include "runtime/xmem_query.h"
#include "storage/dstorage_factory_host.h"
#include "storage/dstorage_async_queue.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
namespace {
std::string utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}
std::string escape(const std::string& raw) {
    std::string out;
    for (unsigned char c : raw) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                const char* hex = "0123456789abcdef";
                out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
            } else out += static_cast<char>(c);
        }
    }
    return out;
}
void write_json(const fs::path& path, const scarlett::PeDetails& d, const fs::path& directory) {
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("Cannot open JSON report output path");
    file << "{\n  \"format\": \"scarlettvi-pe-v1\",\n"
         << "  \"machine\": " << d.machine << ",\n"
         << "  \"preferred_base\": " << d.preferred_base << ",\n"
         << "  \"image_size\": " << d.image_size << ",\n"
         << "  \"entry_rva\": " << d.entry_rva << ",\n"
         << "  \"has_tls\": " << (d.tls_rva ? "true" : "false") << ",\n"
         << "  \"has_exception_directory\": " << (d.exception_rva ? "true" : "false") << ",\n"
         << "  \"sections\": [\n";
    for (size_t i = 0; i < d.sections.size(); ++i) {
        const auto& s = d.sections[i];
        file << "    {\"name\": \"" << escape(s.name)
             << "\", \"rva\": " << s.rva
             << ", \"virtual_size\": " << s.virtual_size
             << ", \"file_offset\": " << s.file_offset
             << ", \"file_size\": " << s.file_size << "}"
             << (i + 1 == d.sections.size() ? "\n" : ",\n");
    }
    file << "  ],\n  \"imports\": [\n";
    for (size_t i = 0; i < d.imports.size(); ++i) {
        const auto& module = d.imports[i];
        file << "    {\"module\": \"" << escape(module.name)
             << "\", \"route\": \"" << scarlett::route_name(scarlett::classify_import(module.name, directory))
             << "\", \"delay_load\": " << (module.delay_loaded ? "true" : "false")
             << ", \"symbols\": [";
        for (size_t j = 0; j < module.symbols.size(); ++j) {
            const auto& symbol = module.symbols[j];
            file << "{\"iat_rva\":" << symbol.iat_rva;
            if (symbol.by_ordinal)
                file << ",\"ordinal\":" << symbol.ordinal;
            else
                file << ",\"name\":\"" << escape(symbol.name) << "\"";
            file << "}" << (j + 1 == module.symbols.size() ? "" : ",");
        }
        file << "]}" << (i + 1 == d.imports.size() ? "\n" : ",\n");
    }
    file << "  ]\n}\n";
    if (!file) throw std::runtime_error("Failed to write complete JSON report");
}
void usage() {
    std::wcout << L"ScarlettVI | native Xbox Series compatibility research\n"
               << L"Usage:\n"
               << L"  ScarlettVI.exe inspect <title.exe> [--json <report.json>]\n"
               << L"  ScarlettVI.exe xg-calls <title.exe>\n"
               << L"  ScarlettVI.exe xg-context <title.exe>\n"
               << L"  ScarlettVI.exe xg-calls-fixture\n"
               << L"  ScarlettVI.exe xg-field-fixture\n"
               << L"  ScarlettVI.exe xg-vcalls-fixture\n"
               << L"  ScarlettVI.exe xg-vcall-links-fixture\n"
               << L"  ScarlettVI.exe xg-vcall-args-fixture\n"
               << L"  ScarlettVI.exe xg-vcall-stack-fixture\n"
               << L"  ScarlettVI.exe xg-vcall-regs-fixture\n"
               << L"  ScarlettVI.exe xg-vcall-followups-fixture\n"
               << L"  ScarlettVI.exe xg-vcall-reads-fixture\n"
               << L"  ScarlettVI.exe xmem-tools-fixture\n"
               << L"  ScarlettVI.exe xmem-stats-fixture\n"
               << L"  ScarlettVI.exe xmem-transfer-fixture\n"
               << L"  ScarlettVI.exe storage-factory-fixture\n"
               << L"  ScarlettVI.exe storage-async-fixture\n"
               << L"  ScarlettVI.exe audio-memory-fixture\n"
               << L"  ScarlettVI.exe audio-address-fixture\n"
               << L"  ScarlettVI.exe audio-shape-fixture\n"
               << L"  ScarlettVI.exe audio-hal-fixture\n"
               << L"  ScarlettVI.exe audio-xapu-fixture\n"
               << L"  ScarlettVI.exe audio-dsp-fixture\n"
               << L"  ScarlettVI.exe audio-xapu-dsp-fixture\n"
               << L"  ScarlettVI.exe synthetic-entry-fixture\n"
               << L"  ScarlettVI.exe synthetic-dll-fixture\n"
               << L"  ScarlettVI.exe xg-vcalls <title.exe>\n"
               << L"  ScarlettVI.exe map <title.exe> [--json <report.json>]\n"
               << L"  ScarlettVI.exe readiness <title.exe> [--json <readiness.json>]\n"
               << L"  ScarlettVI.exe archives <title-directory-or-archive.rpf>\n"
               << L"  ScarlettVI.exe gpu\n"
               << L"  ScarlettVI.exe gpu-transfer\n"
               << L"  ScarlettVI.exe gpu-compute\n"
               << L"  ScarlettVI.exe gpu-texture-shader\n"
               << L"  ScarlettVI.exe gpu-memory-bridge\n"
               << L"  ScarlettVI.exe gpu-device11\n"
               << L"  ScarlettVI.exe gpu-device-factory\n"
               << L"  ScarlettVI.exe memory\n"
               << L"  ScarlettVI.exe memory-scatter\n"
               << L"  ScarlettVI.exe memory-sparse\n"
               << L"  ScarlettVI.exe gpu-sparse-va\n"
               << L"  ScarlettVI.exe gpu-tile-map\n"
               << L"  ScarlettVI.exe gpu-tile-batch\n"
               << L"  ScarlettVI.exe gpu-xmem-tiles\n"
               << L"  ScarlettVI.exe gpu-page-registry\n"
               << L"  ScarlettVI.exe gpu-page-batch\n"
               << L"  ScarlettVI.exe gpu-page-scattered\n"
               << L"  ScarlettVI.exe gpu-texture-layout\n"
               << L"  ScarlettVI.exe gpu-texture-detile\n"
               << L"  ScarlettVI.exe gpu-vmem-config [3|4]\n"
               << L"\nmap is a data-only PE memory mapping/import probe; it DOES NOT execute title code.\n";
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { usage(); return 0; }
    try {
        const std::wstring verb = argv[1];
        if (verb == L"storage-async-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::storage::verify_host_dstorage_async_queue_fixture();
            std::cout << "ScarlettVI | bounded real Win32 async host file queue: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nExact generated file readbacks, FIFO completions, "
                      << "concurrent admissions, factory staging lease."
                      << " Xbox COM queues and title code remain unsupported.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"storage-factory-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::storage::verify_dstorage_factory_fixture();
            std::cout << "ScarlettVI | partial Xbox DirectStorage factory"
                      << " / real owned host staging: "
                      << (passed ? "PASS" : "FAIL") << "\n"
                      << "Public COM slot +0x40 and 32MiB allocation;"
                      << " queues, file IO and decompression remain unsupported.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"synthetic-dll-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::verify_synthetic_dll_lifecycle_fixture();
            std::cout << "ScarlettVI | generated PE64 DLL lifecycle: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nGenerated TLS callback and DllMain ordering, "
                      << "private host TLS for managed worker threads. "
                      << "No original game code executed.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"synthetic-entry-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed = scarlett::verify_synthetic_entry_fixture();
            std::cout << "ScarlettVI | generated x64 PE entrypoint: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nGenerated code only; imported Kernel32 API, relocations, "
                      << "TLS and entrypoint verified. Original title not executed.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"audio-xapu-dsp-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::audio::verify_host_xapu_dsp_integration_fixture();
            std::cout << "ScarlettVI | real host FIR -> XAPU completion FIFO: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nOnly original host-typed commands are processed. "
                      << "Xbox XApuEnqueueCommand remains unbound; title BLOCKED.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"audio-dsp-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::audio::verify_host_dsp_convolution_fixture();
            std::cout << "ScarlettVI | real bounded CPU FIR DSP convolution: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nGenerated interleaved audio only; "
                      << "no Xbox CHAD processing or title execution.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"audio-xapu-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed = scarlett::audio::verify_host_xapu_fixture();
            std::cout << "ScarlettVI | partial host XAPU connection/result queue: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nReal owned queue state. Xbox guest command submission "
                      << "and DSP processing remain UNIMPLEMENTED; title BLOCKED.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"audio-hal-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::audio::verify_host_hal_factory_fixture();
            std::cout << "ScarlettVI | owned AcpHal guest object fixture: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nReal bounded host rings; DSP configuration method "
                      << "returns E_NOTIMPL; title execution remains BLOCKED.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"audio-shape-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::audio::verify_apu_shape_fixtures();
            std::cout << "ScarlettVI | owned SHAPE context storage: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nBounded verified family-4 host buffers; "
                      << "no Xbox DSP or game code executed.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"audio-address-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::audio::verify_host_apu_address_fixtures();
            std::cout << "ScarlettVI | owned APU 32-bit address translation: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nNo foreign memory, Xbox DSP, or game code executed.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"audio-memory-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::audio::verify_host_apu_allocation_fixtures();
            std::cout << "ScarlettVI | host APU alloc/free fixture: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nActual aligned host storage and ownership; "
                      << "no Xbox DSP, APU mapping or title execution.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"xmem-transfer-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::xmem::verify_xmem_transfer_fixture();
            std::cout << "ScarlettVI | bounded XMem title/system quota transfer: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nReal host XMemAlloc quota admission and release; "
                      << "no Xbox physical partition migration or title execution.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"xmem-stats-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::xmem::verify_xmem_stats_failure_fixture();
            std::cout << "ScarlettVI | safe XMem statistics failure-path fixture: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nNo Xbox budget is provided, output storage remains untouched; "
                      << "title startup still BLOCKED.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"xmem-tools-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::xmem::verify_xmem_tools_capability_fixture();
            std::cout << "ScarlettVI | host-only XMem tools-partition "
                      << "capability fixture: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nDesktop policy returns FALSE; no Xbox tools "
                      << "partition is provided or title code executed.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"xg-vcall-reads-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::analysis::verify_xg_postcall_indexed_read_fixtures();
            std::cout << "ScarlettVI | synthetic XG indexed stack-read fixture: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nRead-only local instruction patterns; "
                      << "no output-buffer ownership or Xbox ABI inferred.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"xg-vcall-followups-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::analysis::verify_xg_dispatch_successor_fixtures();
            std::cout << "ScarlettVI | generated XG successor-call fixture: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nRead-only byte patterns, not proven object "
                      << "identity or a release/ownership contract.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"xg-vcall-regs-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::analysis::verify_xg_register_pattern_fixtures();
            std::cout << "ScarlettVI | synthetic XG register timeline fixture: "
                      << (passed ? "PASS" : "FAIL")
                      << "\nOnly local generated instruction patterns; "
                      << "not guest execution or a proven Xbox ABI.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"xg-vcall-stack-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::analysis::verify_xg_postcall_stack_write_fixtures();
            std::cout << "ScarlettVI | synthetic XG outgoing stack-write "
                      << "fixture: " << (passed ? "PASS" : "FAIL")
                      << "\nGenerated MOV-to-RSP patterns only; "
                      << "no game code executed or Xbox ABI inferred.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"xg-vcall-args-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::analysis::verify_xg_dispatch_argument_setup_fixtures();
            std::cout << "ScarlettVI | generated XG dispatch argument "
                      << "signature fixture: " << (passed ? "PASS" : "FAIL")
                      << "\nBounded MOV/LEA patterns only; "
                      << "no Xbox method ABI inferred.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"xg-vcall-links-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::analysis::verify_xg_table_load_fixtures();
            std::cout << "ScarlettVI | synthetic XG table-register "
                      << "load-signature fixture: "
                      << (passed ? "PASS" : "FAIL") << "\n"
                      << "Bounded REX.W MOV byte patterns only; "
                      << "no Xbox object ABI claimed.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"xg-vcalls-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::analysis::verify_xg_indirect_postcall_fixtures();
            std::cout << "ScarlettVI | generated x64 indirect-call candidate fixture: "
                      << (passed ? "PASS" : "FAIL") << "\n"
                      << "Byte patterns, not confirmed XG object methods; "
                      << "no title code executed.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"xg-vcalls") {
            if (argc != 3) { usage(); return 2; }
            const scarlett::PeFile title(argv[2]);
            scarlett::analysis::XgCallsiteReport report;
            std::vector<scarlett::analysis::XgCallContext> contexts;
            if (!scarlett::analysis::probe_xg_callsites(title, report) ||
                !scarlett::analysis::probe_xg_call_contexts(
                    title, report, contexts, 0, 128)) {
                std::cerr << "Read-only XG post-call extraction failed.\n";
                return 3;
            }
            std::vector<scarlett::analysis::XgCallContext> create_contexts;
            for (const auto& context : contexts)
                if (context.symbol == "XGCreateTextureComputer")
                    create_contexts.push_back(context);
            std::vector<scarlett::analysis::XgIndirectCallCandidate> found;
            if (!scarlett::analysis::scan_xg_indirect_postcalls(
                    create_contexts, found)) {
                std::cerr << "XG indirect-call candidate scan failed.\n";
                return 3;
            }
            std::cout << "ScarlettVI | read-only post-create indirect-call "
                      << "byte patterns: " << found.size() << "\n";
            for (const auto& hit : found)
                std::cout << "  XG call VA 0x" << std::hex
                          << (report.image_base + hit.source_call_rva)
                          << " | candidate VA 0x"
                          << (report.image_base + hit.candidate_rva)
                          << " | base GPR " << std::dec
                          << static_cast<unsigned>(hit.base_register)
                          << " | displacement +0x" << std::hex
                          << hit.displacement << std::dec << "\n";
            std::vector<scarlett::analysis::XgTableLoadCandidate> loads;
            if (!scarlett::analysis::scan_xg_table_load_candidates(
                    create_contexts, found, loads)) {
                std::cerr << "Bounded XG table-load survey failed.\n";
                return 3;
            }
            std::cout << "Local table-register load signatures: "
                      << loads.size() << "/" << found.size() << "\n";
            for (const auto& load : loads)
                std::cout << "  indirect call VA 0x" << std::hex
                          << (report.image_base + load.indirect_call_rva)
                          << " | nearby load VA 0x"
                          << (report.image_base + load.load_rva)
                          << std::dec << " | pointer GPR "
                          << static_cast<unsigned>(load.source_pointer_register)
                          << " -> table GPR "
                          << static_cast<unsigned>(load.table_register)
                          << " | byte gap "
                          << static_cast<unsigned>(load.distance_to_call_bytes)
                          << "\n";
            std::vector<scarlett::analysis::XgArgSetupCandidate> arguments;
            if (!scarlett::analysis::scan_xg_dispatch_argument_setups(
                    create_contexts, found, arguments)) {
                std::cerr << "Bounded XG argument-setup survey failed.\n";
                return 3;
            }
            std::cout << "Local argument-setup signatures: "
                      << arguments.size() << "\n";
            for (const auto& arg : arguments)
                std::cout << "  indirect VA 0x" << std::hex
                          << (report.image_base + arg.indirect_call_rva)
                          << " | setup VA 0x"
                          << (report.image_base + arg.setup_rva)
                          << " | "
                          << (arg.form == scarlett::analysis::XgArgSetupForm::mov_rcx_from_memory
                              ? "MOV RCX,[base+disp]" : "LEA RDX,[RSP+disp]")
                          << " | base GPR " << std::dec
                          << static_cast<unsigned>(arg.memory_base_register)
                          << " | +0x" << std::hex << arg.displacement
                          << std::dec << " | gap "
                          << static_cast<unsigned>(arg.bytes_between_setup_and_call)
                          << "\n";
            std::vector<scarlett::analysis::XgStackWriteCandidate> stack_writes;
            if (!scarlett::analysis::scan_xg_postcall_stack_writes(
                    create_contexts, found, stack_writes)) {
                std::cerr << "Bounded XG stack-write survey failed.\n";
                return 3;
            }
            std::cout << "Local outgoing stack-write signatures: "
                      << stack_writes.size() << "\n";
            for (const auto& store : stack_writes)
                std::cout << "  indirect VA 0x" << std::hex
                          << (report.image_base + store.indirect_call_rva)
                          << " | store VA 0x"
                          << (report.image_base + store.store_rva)
                          << " | [RSP+0x" << store.stack_offset << "]"
                          << std::dec << " <= GPR "
                          << static_cast<unsigned>(store.source_register)
                          << " | width " << static_cast<unsigned>(store.width_bytes)
                          << " bytes | gap "
                          << static_cast<unsigned>(store.gap_to_call_bytes)
                          << "\n";
            std::vector<scarlett::analysis::XgRegisterPattern> timeline;
            if (!scarlett::analysis::scan_xg_local_register_patterns(
                    create_contexts, found, timeline)) {
                std::cerr << "Bounded XG register-timeline scan failed.\n";
                return 3;
            }
            std::cout << "Local +0x50 register setup patterns: "
                      << timeline.size() << "\n";
            for (const auto& p : timeline) {
                std::cout << "  indirect VA 0x" << std::hex
                          << (report.image_base + p.indirect_call_rva)
                          << " | setup VA 0x"
                          << (report.image_base + p.instruction_rva)
                          << std::dec << " | destination GPR "
                          << static_cast<unsigned>(p.destination_gpr);
                if (p.kind == scarlett::analysis::XgRegPatternKind::stack_address)
                    std::cout << " <- stack address [RSP+0x"
                              << std::hex << p.stack_offset << std::dec << "]";
                else
                    std::cout << " <- GPR "
                              << static_cast<unsigned>(p.source_gpr);
                std::cout << "\n";
            }
            for (const auto& call : found) {
                if (call.displacement != 0x50)
                    continue;
                for (uint8_t reg : {uint8_t{2}, uint8_t{8}, uint8_t{9}}) {
                    uint32_t offset = 0;
                    if (!scarlett::analysis::resolve_xg_stack_address_pattern(
                            timeline, call.source_call_rva,
                            call.candidate_rva, reg, call.candidate_rva,
                            offset))
                        continue;
                    std::cout << "  LOCAL CHAIN at indirect VA 0x" << std::hex
                              << (report.image_base + call.candidate_rva)
                              << std::dec << ": GPR "
                              << static_cast<unsigned>(reg)
                              << " <- stack-address pattern +0x"
                              << std::hex << offset << std::dec << "\n";
                }
            }
            for (const auto& store : stack_writes) {
                uint32_t offset = 0;
                if (!scarlett::analysis::resolve_xg_stack_address_pattern(
                        timeline, store.source_call_rva,
                        store.indirect_call_rva, store.source_register,
                        store.store_rva, offset))
                    continue;
                std::cout << "  LOCAL CHAIN before store VA 0x" << std::hex
                          << (report.image_base + store.store_rva)
                          << ": source GPR "
                          << std::dec
                          << static_cast<unsigned>(store.source_register)
                          << " <- stack-address pattern +0x"
                          << std::hex << offset << std::dec << "\n";
            }
            std::vector<scarlett::analysis::XgDispatchSuccessorEvidence> successors;
            if (!scarlett::analysis::scan_xg_dispatch_successors(
                    create_contexts, found, successors)) {
                std::cerr << "Bounded XG successor-call survey failed.\n";
                return 3;
            }
            std::cout << "Local immediate +0x08 successor patterns: "
                      << successors.size() << "\n";
            for (const auto& item : successors)
                std::cout << "  first indirect VA 0x" << std::hex
                          << (report.image_base + item.first_indirect_rva)
                          << " (+0x" << item.first_dispatch_offset << ")"
                          << " | successor VA 0x"
                          << (report.image_base + item.next_indirect_rva)
                          << " (+0x08)"
                          << " | RCX reload VA 0x"
                          << (report.image_base + item.reload_rcx_rva)
                          << " | source GPR " << std::dec
                          << static_cast<unsigned>(item.reload_base_gpr)
                          << " | storage displacement +0x" << std::hex
                          << item.reload_displacement << std::dec
                          << " | prior matching location pattern: "
                          << (item.prior_matching_storage_pattern ? "yes" : "no")
                          << "\n";
            std::vector<scarlett::analysis::XgPostcallIndexedRead> reads;
            if (!scarlett::analysis::scan_xg_postcall_indexed_reads(
                    create_contexts, found, reads)) {
                std::cerr << "Bounded XG postcall indexed-read scan failed.\n";
                return 3;
            }
            std::cout << "Local +0x10 postcall indexed stack-read patterns: "
                      << reads.size() << "\n";
            for (const auto& rd : reads)
                std::cout << "  dispatch VA 0x" << std::hex
                          << (report.image_base + rd.dispatch_rva)
                          << " | load VA 0x"
                          << (report.image_base + rd.read_rva)
                          << " | passed RDX [RSP+0x" << rd.passed_rsp_offset
                          << "] | read [RSP+RAX+0x"
                          << rd.indexed_rsp_displacement << "]"
                          << " | relative +0x" << rd.relative_displacement
                          << std::dec << " | width "
                          << static_cast<unsigned>(rd.width_bytes)
                          << " | destination GPR "
                          << static_cast<unsigned>(rd.destination_gpr)
                          << "\n";
            for (const auto& rd : reads) {
                if (!rd.copied_immediately_to_rsi)
                    continue;
                std::cout << "  immediate field-copy VA 0x" << std::hex
                          << (report.image_base + rd.copy_rva)
                          << " | source load VA 0x"
                          << (report.image_base + rd.read_rva)
                          << " | destination [RSI+0x"
                          << rd.rsi_field_offset << "]"
                          << std::dec << " | width "
                          << static_cast<unsigned>(rd.width_bytes)
                          << " bytes\n";
            }
            std::cout << "These byte-pattern matches do not prove that "
                      << "the callee wrote the stack data or an Xbox ABI.\n";
            return 0;
        }
        if (verb == L"xg-field-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed =
                scarlett::analysis::verify_xg_field_projection_fixtures();
            std::cout << "ScarlettVI | synthetic XG field-transfer research fixture: "
                      << (passed ? "PASS" : "FAIL") << "\n"
                      << "512 generated input cases across all three "
                      << "observed category paths, plus bounds and overlaps.\n"
                      << "No Xbox import bound or game code executed.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"xg-calls-fixture") {
            if (argc != 2) { usage(); return 2; }
            const bool passed = scarlett::analysis::verify_xg_callsite_fixtures() &&
                                scarlett::analysis::verify_xg_context_fixtures();
            std::cout << "ScarlettVI | generated XG PE callsite scanner fixture: "
                      << (passed ? "PASS" : "FAIL") << "\n"
                      << "Generated x64 FF25/E8 byte patterns only;"
                      << " no Xbox import code executed.\n";
            return passed ? 0 : 3;
        }
        if (verb == L"xg-calls") {
            if (argc != 3) { usage(); return 2; }
            // Parses input PE bytes only; no image is mapped into executable
            // memory, no DllMain/TLS/entry point or Xbox import is invoked.
            const scarlett::PeFile title(argv[2]);
            scarlett::analysis::XgCallsiteReport report;
            if (!scarlett::analysis::probe_xg_callsites(title, report)) {
                std::cerr << "XG read-only PE scan failed.\n";
                return 3;
            }
            std::cout << "ScarlettVI | XG static PE callsite candidates"
                      << " | executable bytes scanned: "
                      << report.executable_bytes_scanned
                      << " | preferred image base: 0x"
                      << std::hex << report.image_base << std::dec << "\n";
            for (const auto& symbol : report.symbols) {
                std::cout << symbol.symbol
                          << " | import: " << (symbol.imported ? "yes" : "no")
                          << " | IAT RVA: 0x" << std::hex << symbol.iat_rva
                          << std::dec
                          << " | FF25 thunk matches: "
                          << symbol.trampoline_rvas.size()
                          << " | E8 rel32 call candidates: "
                          << symbol.direct_rel32_call_rvas.size() << "\n";
                for (auto stub : symbol.trampoline_rvas)
                    std::cout << "  thunk VA 0x" << std::hex
                              << (report.image_base + stub)
                              << " -> IAT VA 0x"
                              << (report.image_base + symbol.iat_rva)
                              << std::dec << "\n";
                for (auto rva : symbol.direct_rel32_call_rvas)
                    std::cout << "  candidate call VA 0x" << std::hex
                              << (report.image_base + rva) << std::dec << "\n";
            }
            std::cout << "Byte-pattern candidates, not a full disassembler."
                      << " Xbox texture descriptors, swizzles and object"
                      << " methods remain unverified; title execution BLOCKED.\n";
            return 0;
        }
        if (verb == L"xg-context") {
            if (argc != 3) { usage(); return 2; }
            // Read-only per-call executable-file windows for independent
            // disassembly. Never maps or executes the title.
            const scarlett::PeFile title(argv[2]);
            scarlett::analysis::XgCallsiteReport report;
            std::vector<scarlett::analysis::XgCallContext> contexts;
            if (!scarlett::analysis::probe_xg_callsites(title, report) ||
                !scarlett::analysis::probe_xg_call_contexts(
                    title, report, contexts)) {
                std::cerr << "XG executable context verification failed.\n";
                return 3;
            }
            auto print_hex = [](const std::vector<uint8_t>& bytes) {
                for (uint8_t byte : bytes)
                    std::cout << std::hex << std::setw(2)
                              << std::setfill('0')
                              << static_cast<unsigned>(byte) << ' ';
                std::cout << std::dec << std::setfill(' ') << "\n";
            };
            std::cout << "ScarlettVI | bounded read-only XG call contexts: "
                      << contexts.size() << " | executable PE bytes only\n";
            for (const auto& ctx : contexts) {
                std::cout << ctx.symbol << " | call VA 0x" << std::hex
                          << report.image_base + ctx.call_rva
                          << " -> thunk VA 0x"
                          << report.image_base + ctx.target_trampoline_rva
                          << " | section RVA 0x" << ctx.section_rva
                          << std::dec << "\n";
                std::cout << "  pre (" << ctx.before.size() << " bytes): ";
                print_hex(ctx.before);
                std::cout << "  E8 call: ";
                print_hex(std::vector<uint8_t>(
                    ctx.call_bytes.begin(), ctx.call_bytes.end()));
                std::cout << "  post (" << ctx.after.size() << " bytes): ";
                print_hex(ctx.after);
            }
            std::cout << "Raw byte windows are NOT disassembly or ABI proof. "
                      << "No import called; Xbox graphics still unresolved.\n";
            return 0;
        }
        if (verb == L"gpu-page-scattered") {
            if (argc != 2) { usage(); return 2; }
            scarlett::gpu::DeviceContext gpu;
            const HRESULT hr = gpu.guest_gpu_page_scattered_test();
            const size_t pages =
                scarlett::memory::PhysicalMemory::instance().allocated_page_count();
            const size_t pins =
                scarlett::memory::PhysicalMemory::instance().pinned_alias_count();
            const bool pass = SUCCEEDED(hr) &&
                gpu.scattered_run_count() == 4 &&
                gpu.submitted_commands() == 24 &&
                gpu.registry_queue_submissions() == 3 &&
                gpu.registry_batch_mapping_updates() == 22 &&
                gpu.registry_grouped_unmapping_updates() == 22 &&
                pages == 0 && pins == 0;
            std::wcout << L"ScarlettVI | scattered host GPU page planner: "
                       << (pass ? L"PASS" : L"FAIL")
                       << L"\nGPU: " << gpu.adapter_name() << L"\n";
            std::cout << "8 XMem frames / 4 planned native-resource runs"
                      << " | completed GPU page transfers: "
                      << gpu.submitted_commands()
                      << " | GPU command submissions: "
                      << gpu.registry_queue_submissions()
                      << " | native map updates: "
                      << gpu.registry_batch_mapping_updates()
                      << " | native NULL unmap updates: "
                      << gpu.registry_grouped_unmapping_updates()
                      << " | leaked physical pages: " << pages
                      << " | retained CPU pins: " << pins
                      << " | HRESULT 0x" << std::hex
                      << static_cast<uint32_t>(hr) << std::dec << "\n"
                      << "Generated Windows D3D12 fixture only;"
                      << " Xbox GPU page tables remain unsupported.\n";
            return pass ? 0 : 3;
        }
        if (verb == L"gpu-page-batch") {
            if (argc != 2) { usage(); return 2; }
            scarlett::gpu::DeviceContext gpu;
            const HRESULT hr = gpu.guest_gpu_page_batch_test();
            const size_t pages =
                scarlett::memory::PhysicalMemory::instance().allocated_page_count();
            const size_t pins =
                scarlett::memory::PhysicalMemory::instance().pinned_alias_count();
            const bool pass = SUCCEEDED(hr) &&
                gpu.submitted_commands() == 20 &&
                gpu.registry_queue_submissions() == 5 &&
                gpu.registry_batch_mapping_updates() == 10 &&
                gpu.registry_grouped_unmapping_updates() == 4 &&
                pages == 0 && pins == 0;
            std::wcout << L"ScarlettVI | four-frame pinned GPU tile batch: "
                       << (pass ? L"PASS" : L"FAIL")
                       << L"\nGPU: " << gpu.adapter_name() << L"\n";
            std::cout << "4 XMem frames / 8 CPU VA pins / 8 GPU aliases"
                      << " | completed GPU page transfers: " << gpu.submitted_commands()
                      << " | GPU command queue submissions: "
                      << gpu.registry_queue_submissions()
                      << " | native tile map calls: "
                      << gpu.registry_batch_mapping_updates()
                      << " | grouped native tile unmap calls: "
                      << gpu.registry_grouped_unmapping_updates()
                      << " | remaining pages: " << pages
                      << " | remaining pins: " << pins
                      << " | HRESULT 0x" << std::hex
                      << static_cast<uint32_t>(hr) << std::dec << "\n"
                      << "Mapping failure is synthetic; real GPU work is fenced"
                      << " before releasing CPU pins and tile backing.\n";
            return pass ? 0 : 3;
        }
        if (verb == L"gpu-page-registry") {
            if (argc != 2) { usage(); return 2; }
            scarlett::gpu::DeviceContext gpu;
            const HRESULT hr = gpu.guest_gpu_page_registry_test();
            const size_t pages =
                scarlett::memory::PhysicalMemory::instance().allocated_page_count();
            const size_t pins =
                scarlett::memory::PhysicalMemory::instance().pinned_alias_count();
            const bool pass = SUCCEEDED(hr) &&
                pages == 0 && pins == 0 && gpu.submitted_commands() == 17 &&
                gpu.registry_queue_submissions() == 10;
            std::wcout << L"ScarlettVI | pinned guest page registry + real D3D12"
                       << L" tile staging: " << (pass ? L"PASS" : L"FAIL")
                       << L"\nGPU: " << gpu.adapter_name() << L"\n";
            std::cout << "4 guest frames / 8 CPU aliases / 8 GPU mappings"
                      << " / 4 distinct 64KiB tiles in one 4MiB D3D12 heap"
                      << " | GPU page transfer results: "
                      << gpu.submitted_commands()
                      << " | native ExecuteCommandLists calls: "
                      << gpu.registry_queue_submissions()
                      << " | leaked CPU pages: " << pages
                      << " | pinned aliases left: " << pins
                      << " | HRESULT 0x" << std::hex
                      << static_cast<uint32_t>(hr) << std::dec << "\n"
                      << "Host GPU heap tiles are separately backed;"
                      << " Xbox GPU page tables and unified coherency remain unsupported.\n";
            return pass ? 0 : 3;
        }
        if (verb == L"gpu-xmem-tiles") {
            if (argc != 2) { usage(); return 2; }
            scarlett::gpu::DeviceContext context;
            const HRESULT hr = context.staged_xmem_tile_roundtrip_test();
            const size_t live_pages =
                scarlett::memory::PhysicalMemory::instance().allocated_page_count();
            const bool pass = SUCCEEDED(hr) && live_pages == 0 &&
                context.submitted_commands() == 3;
            std::wcout << L"ScarlettVI | XMem CPU page aliases to real"
                       << L" 16GiB-window D3D12 GPU tiles: "
                       << (pass ? L"PASS" : L"FAIL") << L"\n"
                       << L"GPU: " << context.adapter_name() << L"\n";
            std::cout << "64KiB page: 3 generated payloads / 2 independent GPU sessions"
                      << " | GPU-submitted payloads: " << context.submitted_commands()
                      << " | completed final session fence: " << context.staged_completed_fence()
                      << " | leaked physical pages: " << live_pages
                      << " | HRESULT 0x" << std::hex << static_cast<uint32_t>(hr)
                      << std::dec << "\n"
                      << "CPU XMem aliases and GPU tiles are NOT unified memory."
                      << " Explicit upload, fence, readback and CPU writeback only;"
                      << " Xbox XMemTransferMemory remains unsupported.\n";
            return pass ? 0 : 3;
        }
        if (verb == L"gpu-texture-detile") {
            if (argc != 2) { usage(); return 2; }
            scarlett::gpu::DeviceContext gpu;
            const HRESULT hr = gpu.detile_host_texture_gpu_fixture();
            std::wcout << L"ScarlettVI | real DXIL GPU texture detiling: "
                       << (SUCCEEDED(hr) ? L"PASS" : L"FAIL") << L"\n"
                       << L"GPU: " << gpu.adapter_name() << L"\n";
            std::cout << "20 synthetic D3D12 GPU submissions across"
                      << " 5 DXGI formats, mip chains and odd dimensions"
                      << " | compiled DXIL: " << gpu.detile_dxil_bytes()
                      << " bytes | submissions: " << gpu.submitted_commands()
                      << " | HRESULT: 0x" << std::hex
                      << static_cast<uint32_t>(hr) << std::dec << "\n"
                      << "Host 8x8 Morton microtiles only: NOT Xbox XG/RDNA2"
                      << " swizzle and NOT game shader execution.\n";
            if (FAILED(hr) && !gpu.shader_diagnostics().empty())
                std::cerr << "DXC: " << gpu.shader_diagnostics() << "\n";
            return SUCCEEDED(hr) && gpu.submitted_commands() == 20 ? 0 : 3;
        }
        if (verb == L"gpu-texture-layout") {
            if (argc != 2) { usage(); return 2; }
            uint64_t bytes_checked = 0;
            uint32_t subresources_checked = 0;
            const bool cpu_ok = scarlett::gpu::verify_texture_layout_fixtures(
                &bytes_checked, &subresources_checked);
            scarlett::gpu::DeviceContext gpu;
            HRESULT hr = cpu_ok ? gpu.initialize() : E_FAIL;
            if (SUCCEEDED(hr)) hr = gpu.texture_layout_gpu_test();
            std::cout << "ScarlettVI | generated format-aware host texture layouts"
                      << " + real D3D12 texture mips: "
                      << (SUCCEEDED(hr) ? "PASS" : "FAIL") << "\n"
                      << "Formats: R8, RGBA8, BC1, BC3, BC5"
                      << " | CPU subresources: " << subresources_checked
                      << " | exact CPU decoded bytes: " << bytes_checked
                      << " | GPU subresources checked: "
                      << (SUCCEEDED(hr) ? 40 : 0)
                      << " | GPU submissions: " << gpu.submitted_commands()
                      << " | HRESULT: 0x" << std::hex << static_cast<uint32_t>(hr)
                      << std::dec << "\n"
                      << "Original synthetic 8x8 Morton microtiles, NOT Xbox"
                      << " XG swizzle or console texture descriptors.\n";
            return SUCCEEDED(hr) ? 0 : 3;
        }
        if (verb == L"gpu-tile-batch") {
            if (argc != 2) { usage(); return 2; }
            scarlett::gpu::DeviceContext gpu;
            const HRESULT hr = gpu.tile_batch_mapping_test();
            const bool pass = SUCCEEDED(hr) &&
                gpu.submitted_commands() == 2 &&
                gpu.tile_mapping_updates() == 4 &&
                gpu.tile_unmapping_updates() == 4;
            std::wcout << L"ScarlettVI | native D3D12 multi-range tile mapping: "
                       << (pass ? L"PASS" : L"FAIL")
                       << L"\nGPU: " << gpu.adapter_name() << L"\n";
            std::cout << "Four 64KiB physical tiles / two distant guest aliases"
                      << " | verified GPU copies: " << gpu.submitted_commands()
                      << " | native map calls: " << gpu.tile_mapping_updates()
                      << " | native unmap calls: " << gpu.tile_unmapping_updates()
                      << " | HRESULT: 0x" << std::hex
                      << static_cast<uint32_t>(hr) << std::dec << "\n"
                      << "Host D3D12 tile mapping only; not Xbox GPU page tables.\n";
            return pass ? 0 : 3;
        }
        if (verb == L"gpu-tile-map") {
            if (argc != 2) { usage(); return 2; }
            // Only this standalone diagnostic fixture enables the host
            // Windows D3D12 debug layer, when installed on the machine.
            Microsoft::WRL::ComPtr<ID3D12Debug> debug;
            const HRESULT debug_hr =
                D3D12GetDebugInterface(IID_PPV_ARGS(debug.GetAddressOf()));
            if (SUCCEEDED(debug_hr)) debug->EnableDebugLayer();
            std::cout << "D3D12 host debug layer: "
                      << (debug ? "enabled" : "unavailable") << "\n";
            scarlett::gpu::DeviceContext context;
            HRESULT hr = context.initialize();
            if (SUCCEEDED(hr)) hr = context.tiled_buffer_alias_test();
            std::wcout << L"ScarlettVI | native D3D12 64KiB GPU tile mapping"
                       << L" + cross-resource physical alias readback: "
                       << (SUCCEEDED(hr) ? L"PASS" : L"FAIL") << L"\n"
                       << L"Host GPU: " << context.adapter_name() << L"\n";
            std::cout << "D3D12 UpdateTileMappings: 2 large sparse buffers ->"
                      << " one shared heap tile | 65536 bytes GPU-verified"
                      << " | submissions: " << context.submitted_commands()
                      << " | completed fence: " << context.completed_fence()
                      << " | HRESULT 0x" << std::hex << static_cast<uint32_t>(hr)
                      << std::dec << "\n"
                      << "Guest offsets 4GiB and 12GiB translate into separate native 4GiB segments.\n"
                       << "Real host GPU tile residency only; this does NOT implement"
                      << " Xbox GPU page tables, tiling formats or shader translation.\n";
            return SUCCEEDED(hr) ? 0 : 3;
        }
        if (verb == L"gpu-vmem-config") {
            if (argc != 2 && argc != 3) { usage(); return 2; }
            uint32_t mode = 4;
            if (argc == 3) {
                if (std::wstring_view(argv[2]) == L"3") mode = 3;
                else if (std::wstring_view(argv[2]) != L"4") { usage(); return 2; }
            }
            bool pass = scarlett::gpu::verify_xbox_gpu_virtual_memory_configuration(mode);
            // The title performs CPU VA reservations immediately after its
            // Xbox GPU configuration call; validate that full sequence using
            // the same observed 24GiB range and 64KiB reserve flags.
            const size_t window = static_cast<size_t>(mode == 3 ? 16 : 24) << 30;
            void* cpu_a = pass ? scarlett::xmem::XMemVirtualAlloc(
                nullptr, window, MEM_RESERVE | MEM_64K_PAGES, 0x21, 4) : nullptr;
            void* cpu_b = pass ? scarlett::xmem::XMemVirtualAlloc(
                nullptr, window, MEM_RESERVE | MEM_64K_PAGES, 0x3, 0x801) : nullptr;
            pass = pass && cpu_a && cpu_b && cpu_a != cpu_b;
            if (cpu_a) pass = scarlett::xmem::VirtualFreeBridge(
                cpu_a, 0, MEM_RELEASE) && pass;
            if (cpu_b) pass = scarlett::xmem::VirtualFreeBridge(
                cpu_b, 0, MEM_RELEASE) && pass;
            std::cout << "ScarlettVI | partial Xbox GPU VM config + native Device11"
                      << " + real 2x" << (mode == 3 ? 16 : 24)
                      << "GiB CPU/GPU sparse reservations: "
                      << (pass ? "PASS" : "FAIL") << "\n"
                      << "Observed 32-byte config mode=" << mode
                      << "; host reserved GPU VA"
                      << " without VRAM commit; host CPU VA reserved without RAM commit.\n"
                      << "Xbox tile mappings, console protection rules, and GPU"
                      << " address-space identity are NOT implemented.\n";
            return pass ? 0 : 3;
        }
        if (verb == L"gpu-sparse-va") {
            if (argc != 2) { usage(); return 2; }
            scarlett::gpu::DeviceContext gpu;
            HRESULT hr = gpu.initialize();
            Microsoft::WRL::ComPtr<ID3D12Device11> device;
            if (SUCCEEDED(hr)) hr = gpu.acquire_device11(device.GetAddressOf());
            uint32_t process_bits = 0;
            uint32_t resource_bits = 0;
            for (uint64_t size_gib : {16ull, 24ull}) {
                if (FAILED(hr)) break;
                scarlett::gpu::SparseGpuWindows windows;
                hr = windows.initialize(device.Get(), size_gib * (1ull << 30), 2);
                process_bits = windows.gpu_va_bits_per_process();
                resource_bits = windows.gpu_va_bits_per_resource();
                const size_t expected_segments =
                    static_cast<size_t>(2 * size_gib / 4);
                if (SUCCEEDED(hr) &&
                    (windows.segment_count() != expected_segments ||
                     !windows.verify_boundaries())) hr = E_FAIL;
                windows.clear();
                if (SUCCEEDED(hr) && windows.window_count() != 0) hr = E_FAIL;
            }
            std::wcout << L"ScarlettVI | actual sparse D3D12 GPU VA reservations: "
                       << (SUCCEEDED(hr) ? L"PASS" : L"FAIL") << L"\n"
                       << L"Host GPU: " << gpu.adapter_name() << L"\n";
            std::cout << "GPU VA bits per process: " << process_bits
                      << " | per resource: " << resource_bits
                      << " | tested windows: 2 x 16GiB and 2 x 24GiB"
                       << " | 4GiB native segments: 8 (16GiB), 12 (24GiB)"
                      << " | actual VRAM committed: 0"
                      << " | HRESULT: 0x" << std::hex << static_cast<uint32_t>(hr)
                      << std::dec << "\n"
                      << "This mode reserves unbacked GPU VA only. The gpu-tile-map"
                      << " command verifies host 64KiB tile residency separately; Xbox physical page mapping remains unsupported.\n";
            return SUCCEEDED(hr) ? 0 : 3;
        }
        if (verb == L"memory-sparse") {
            if (argc != 2) { usage(); return 2; }
            constexpr size_t page = scarlett::memory::PhysicalMemory::page_bytes;
            constexpr size_t gib = size_t{1024} * 1024 * 1024;
            auto verify_case = [&](size_t gib_count) -> bool {
                const size_t bytes = gib_count * gib;
                const DWORD flags = MEM_RESERVE | MEM_64K_PAGES;
                void* first = scarlett::xmem::XMemVirtualAlloc(
                    nullptr, bytes, flags, 0, PAGE_READWRITE);
                void* second = scarlett::xmem::XMemVirtualAlloc(
                    nullptr, bytes, flags, 0, PAGE_READWRITE);
                uintptr_t physical[3]{};
                uintptr_t pages_allocated = 3;
                bool pages_valid = scarlett::xmem::XMemAllocatePhysicalPages(
                    MEM_64K_PAGES, &pages_allocated, physical) != 0 &&
                    pages_allocated == 3;
                bool ok = first && second && pages_valid;
                auto* start = static_cast<uint8_t*>(first);
                auto* another = static_cast<uint8_t*>(second);
                void* destinations[5]{
                    start,
                    start ? start + bytes / 2 : nullptr,
                    start ? start + bytes - page : nullptr,
                    another ? another + bytes / 2 : nullptr,
                    another ? another + bytes - page : nullptr
                };
                if (ok) {
                    MEMORY_BASIC_INFORMATION a{}, b{};
                    const bool sparse_first = VirtualQuery(first, &a, sizeof(a)) == sizeof(a) &&
                        a.State == MEM_RESERVE && a.RegionSize >= bytes;
                    const bool sparse_second = VirtualQuery(second, &b, sizeof(b)) == sizeof(b) &&
                        b.State == MEM_RESERVE && b.RegionSize >= bytes;
                    ok = sparse_first && sparse_second;
                }
                const uintptr_t page_ids[5]{
                    physical[0], physical[1], physical[0], physical[0], physical[2]
                };
                bool mapped = false;
                if (ok) {
                    mapped = scarlett::xmem::XMemMapPhysicalPagesScatter(
                        destinations, 5, page_ids) != FALSE;
                    ok = mapped;
                }
                if (ok) {
                    auto* a = static_cast<volatile uint8_t*>(destinations[0]);
                    auto* b = static_cast<volatile uint8_t*>(destinations[2]);
                    auto* c = static_cast<volatile uint8_t*>(destinations[3]);
                    a[0] = 0xA3;
                    a[page - 1] = 0x29;
                    ok = b[0] == 0xA3 && c[0] == 0xA3 &&
                         b[page - 1] == 0x29 && c[page - 1] == 0x29;
                    MEMORY_BASIC_INFORMATION untouched{};
                    void* empty = start + page;
                    ok = ok && VirtualQuery(empty, &untouched, sizeof(untouched)) ==
                               sizeof(untouched) && untouched.State == MEM_RESERVE &&
                         untouched.RegionSize > (gib / 2);
                    // Mapped pages remain owned until all aliases are removed.
                    ok = ok && !scarlett::xmem::XMemFreePhysicalPages(
                        1, physical);
                }
                bool unmapped = !mapped ||
                    scarlett::xmem::XMemMapPhysicalPagesScatter(
                        destinations, 5, nullptr) != FALSE;
                bool released_first = !first || scarlett::xmem::VirtualFreeBridge(
                    first, 0, MEM_RELEASE) != FALSE;
                bool released_second = !second || scarlett::xmem::VirtualFreeBridge(
                    second, 0, MEM_RELEASE) != FALSE;
                bool freed = !pages_valid || scarlett::xmem::XMemFreePhysicalPages(
                    pages_allocated, physical) != FALSE;
                return ok && unmapped && released_first && released_second && freed &&
                    scarlett::memory::PhysicalMemory::instance().allocated_page_count() == 0;
            };
            const bool first_case = verify_case(16);
            const bool second_case = first_case && verify_case(24);
            const bool pass = first_case && second_case;
            std::cout << "ScarlettVI | sparse Xbox-sized placeholder reservations: "
                      << (pass ? "PASS" : "FAIL") << "\n"
                      << "Verified 2 x 16 GiB and 2 x 24 GiB VA windows, 64 KiB on-demand"
                      << " mapping, cross-window physical aliases, 0 leaked pages."
                      << "\nNo 16/24 GiB physical memory or VRAM was committed.\n";
            return pass ? 0 : 3;
        }
        if (verb == L"gpu-device-factory") {
            if (argc != 2) { usage(); return 2; }
            const bool pass = scarlett::gpu::verify_xbox_device_factory();
            std::cout << "ScarlettVI | native Device11 Xbox-factory ABI-shaped bridge: "
                      << (pass ? "PASS" : "FAIL") << "\n"
                      << "Inputs: observed SDK 0x20004, 1MiB ring | real ID3D12Device11 COM"
                      << " | native command queue | reference lifetime"
                      << " | unsupported arguments rejected\n"
                      << "Xbox memory/flags and title startup remain unsupported.\n";
            return pass ? 0 : 3;
        }
        if (verb == L"gpu-device11") {
            if (argc != 2) { usage(); return 2; }
            scarlett::gpu::DeviceContext gpu;
            HRESULT hr = gpu.initialize();
            if (SUCCEEDED(hr)) hr = gpu.device11_contract_test();
            std::wcout << L"ScarlettVI | exact ID3D12Device11 COM host contract: "
                       << (SUCCEEDED(hr) ? L"PASS" : L"FAIL") << L"\n"
                       << L"GPU: " << gpu.adapter_name() << L"\n";
            std::cout << "Requested Xbox Device IID: "
                      << "5405c344-d457-444e-b4dd-2366e45aee39"
                      << " | host IID supports queue + allocation-info API: "
                      << (SUCCEEDED(hr) ? "yes" : "no")
                      << " | HRESULT: 0x" << std::hex << static_cast<uint32_t>(hr)
                      << std::dec << "\n"
                      << "Xbox creation options, GPU page tables and additional interfaces are NOT implemented.\n";
            return SUCCEEDED(hr) ? 0 : 3;
        }
        if (verb == L"gpu-memory-bridge") {
            if (argc != 2) { usage(); return 2; }
            constexpr size_t kPageBytes = scarlett::memory::PhysicalMemory::page_bytes;
            constexpr size_t kWords = 1024;
            constexpr uint64_t kGuestInputVa = 0x400000000000ull;
            constexpr uint64_t kGuestOutputVa = kGuestInputVa + kPageBytes;

            uintptr_t pages[2]{};
            uintptr_t count = 2;
            if (!scarlett::xmem::XMemAllocatePhysicalPages(
                    MEM_64K_PAGES, &count, pages) || count != 2) {
                std::cerr << "Could not allocate independent guest physical pages\n";
                return 3;
            }
            void* main_view = scarlett::xmem::XMemVirtualAlloc(
                nullptr, kPageBytes * 2, MEM_RESERVE | MEM_64K_PAGES, 0, PAGE_READWRITE);
            void* alias_view = scarlett::xmem::XMemVirtualAlloc(
                nullptr, kPageBytes, MEM_RESERVE | MEM_64K_PAGES, 0, PAGE_READWRITE);
            auto cleanup = [&]() {
                bool ok = true;
                if (main_view) ok = scarlett::xmem::VirtualFreeBridge(
                    main_view, 0, MEM_RELEASE) != FALSE && ok;
                if (alias_view) ok = scarlett::xmem::VirtualFreeBridge(
                    alias_view, 0, MEM_RELEASE) != FALSE && ok;
                return (scarlett::xmem::XMemFreePhysicalPages(2, pages) != FALSE) && ok;
            };

            bool ok = main_view && alias_view;
            if (ok) {
                ok = scarlett::xmem::XMemMapPhysicalPages(main_view, 2, pages) == main_view;
                if (ok) {
                    const uintptr_t output_page = pages[1];
                    ok = scarlett::xmem::XMemMapPhysicalPages(
                        alias_view, 1, &output_page) == alias_view;
                }
            }

            scarlett::gpu::DeviceContext gpu;
            scarlett::gpu::GuestAddressSpace gpu_space;
            HRESULT hr = ok ? gpu.initialize() : E_FAIL;
            constexpr uint32_t seeds[2]{0x13579BDFu, 0x2468ACE0u};
            if (SUCCEEDED(hr)) {
                auto* input = static_cast<uint32_t*>(main_view);
                auto* output = reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(main_view) + kPageBytes);
                auto* alias = static_cast<uint32_t*>(alias_view);

                for (uint32_t pass = 0; pass < 2 && ok; ++pass) {
                    for (uint32_t i = 0; i < kWords; ++i) {
                        input[i] = (i * 0x01010101u) ^ (i << (8 + pass)) ^ 0xA11CE55u;
                        output[i] = 0;
                    }
                    hr = gpu.execute_guest_buffer(
                        std::span<const uint32_t>(input, kWords),
                        std::span<uint32_t>(output, kWords),
                        gpu_space, kGuestInputVa, kGuestOutputVa, seeds[pass]);
                    ok = SUCCEEDED(hr) && gpu_space.resource_count() == 0;
                    for (uint32_t i = 0; i < kWords && ok; ++i) {
                        const uint32_t expected =
                            (input[i] ^ 0xA5A5A5A5u) + i * 7u + seeds[pass];
                        ok = output[i] == expected && alias[i] == expected;
                    }
                }
                if (ok) {
                    // Incorrect or overlapping guest GPU VA reservations must
                    // be rejected without retaining any partially registered
                    // D3D12 resources or submitting extra GPU work.
                    const HRESULT overlap = gpu.execute_guest_buffer(
                        std::span<const uint32_t>(input, kWords),
                        std::span<uint32_t>(output, kWords),
                        gpu_space, kGuestInputVa, kGuestInputVa, seeds[0]);
                    const HRESULT misaligned = gpu.execute_guest_buffer(
                        std::span<const uint32_t>(input, kWords),
                        std::span<uint32_t>(output, kWords),
                        gpu_space, kGuestInputVa + 1, kGuestOutputVa, seeds[1]);
                    ok = overlap == E_INVALIDARG && misaligned == E_INVALIDARG &&
                         gpu_space.resource_count() == 0 && gpu.submitted_commands() == 2;
                }
            } else {
                ok = false;
            }
            const bool freed = cleanup();
            ok = ok && freed &&
                scarlett::memory::PhysicalMemory::instance().allocated_page_count() == 0 &&
                gpu_space.resource_count() == 0;
            std::wcout << L"ScarlettVI | guest 64KiB pages -> host DXIL GPU compute -> guest aliases: "
                       << (ok ? L"PASS" : L"FAIL") << L"\n";
            std::wcout << L"Host GPU: " << gpu.adapter_name() << L"\n";
            std::cout << "GPU dispatches: " << gpu.submitted_commands()
                      << " | fence completed: " << gpu.completed_fence()
                      << " | verified guest output words: " << (ok ? 2 * kWords : 0)
                      << " | GPU VA mappings left: " << gpu_space.resource_count()
                      << " | physical pages left: "
                      << scarlett::memory::PhysicalMemory::instance().allocated_page_count()
                      << " | HRESULT: 0x" << std::hex
                      << static_cast<uint32_t>(hr) << std::dec << "\n"
                      << "Synthetic guest addresses and generated HLSL only. "
                      << "Not Xbox GPU page tables or an executed game shader.\n";
            return ok ? 0 : 3;
        }
        if (verb == L"memory-scatter") {
            if (argc != 2) { usage(); return 2; }
            constexpr size_t stride = scarlett::memory::PhysicalMemory::page_bytes;
            std::array<uintptr_t, 3> pages{};
            uintptr_t count = pages.size();
            if (!scarlett::xmem::XMemAllocatePhysicalPages(
                    MEM_64K_PAGES, &count, pages.data()) || count != pages.size()) {
                std::cerr << "Scatter physical-page allocation failed: " << GetLastError() << "\n";
                return 3;
            }
            void* first = scarlett::xmem::XMemVirtualAlloc(
                nullptr, 3 * stride, MEM_RESERVE | MEM_64K_PAGES, 0, PAGE_READWRITE);
            void* second = scarlett::xmem::XMemVirtualAlloc(
                nullptr, 2 * stride, MEM_RESERVE | MEM_64K_PAGES, 0, PAGE_READWRITE);
            // Release existing mappings even after an individual test fails.
            auto cleanup = [&]() {
                bool released = true;
                if (first) released = scarlett::xmem::VirtualFreeBridge(first, 0, MEM_RELEASE) && released;
                if (second) released = scarlett::xmem::VirtualFreeBridge(second, 0, MEM_RELEASE) && released;
                return scarlett::xmem::XMemFreePhysicalPages(count, pages.data()) && released;
            };
            bool ok = first && second;
            void* destinations[3]{
                first,
                second ? static_cast<uint8_t*>(second) + stride : nullptr,
                first ? static_cast<uint8_t*>(first) + 2 * stride : nullptr
            };
            const uintptr_t page_order[3]{pages[0], pages[0], pages[1]};
            if (ok) {
                ok = scarlett::xmem::XMemMapPhysicalPagesScatter(
                    destinations, 3, page_order) != FALSE;
            }
            if (ok) {
                auto* view_a = static_cast<volatile uint8_t*>(destinations[0]);
                auto* view_b = static_cast<volatile uint8_t*>(destinations[1]);
                view_a[0] = 0x6D;
                ok = view_b[0] == 0x6D; // physical alias across different regions
                view_b[7] = 0xB7;
                ok = ok && view_a[7] == 0xB7;
            }
            if (ok) {
                // A mapped page cannot be freed while still aliased.
                ok = !scarlett::xmem::XMemFreePhysicalPages(1, &pages[0]);
                void* vacant = static_cast<uint8_t*>(first) + stride;
                void* duplicates[2]{vacant, vacant};
                const uintptr_t duplicate_pages[2]{pages[2], pages[1]};
                const bool rejected_duplicates =
                    !scarlett::xmem::XMemMapPhysicalPagesScatter(
                        duplicates, 2, duplicate_pages);
                void* invalid[2]{vacant, reinterpret_cast<void*>(uintptr_t{0x10000})};
                const bool rejected_outside =
                    !scarlett::xmem::XMemMapPhysicalPagesScatter(
                        invalid, 2, duplicate_pages);
                const bool vacant_still_available =
                    scarlett::xmem::XMemMapPhysicalPagesScatter(
                        &vacant, 1, &pages[2]) != FALSE;
                ok = ok && rejected_duplicates && rejected_outside && vacant_still_available;
                if (vacant_still_available) {
                    auto* value = static_cast<volatile uint8_t*>(vacant);
                    value[0] = 0x42;
                    ok = ok && value[0] == 0x42;
                    ok = ok && scarlett::xmem::XMemMapPhysicalPagesScatter(
                        &vacant, 1, nullptr) != FALSE;
                }
            }
            if (first && second) {
                // nullptr PFNs are the title's observed scatter unmap path.
                const bool unmapped = scarlett::xmem::XMemMapPhysicalPagesScatter(
                    destinations, 3, nullptr) != FALSE;
                ok = ok && unmapped;
            }
            const bool released = cleanup();
            ok = ok && released &&
                 scarlett::memory::PhysicalMemory::instance().allocated_page_count() == 0;
            std::cout << "ScarlettVI | original 64KiB scatter VA remapping: "
                      << (ok ? "PASS" : "FAIL") << "\n"
                      << "VA regions: 2 | mapped scatter slots: 3 | physical aliases: 2"
                      << " | invalid requests rejected: " << (ok ? "yes" : "not all")
                      << " | total physical pages remaining: "
                      << scarlett::memory::PhysicalMemory::instance().allocated_page_count()
                      << "\n";
            return ok ? 0 : 3;
        }
        if (verb == L"gpu-texture-shader") {
            if (argc != 2) { usage(); return 2; }
            scarlett::gpu::DeviceContext context;
            HRESULT hr = context.initialize();
            if (SUCCEEDED(hr)) hr = context.texture_shader_test();
            if (SUCCEEDED(hr)) hr = context.compute_dispatch_test(0x12345678u);
            if (SUCCEEDED(hr)) hr = context.texture_shader_test();
            std::wcout << L"ScarlettVI | D3D12 DXIL texture SRV/UAV processing: "
                       << (SUCCEEDED(hr) ? L"PASS" : L"FAIL") << L"\n";
            std::wcout << L"GPU: " << context.adapter_name() << L"\n";
            std::cout << "Texture DXIL bytes: " << context.texture_dxil_bytes()
                      << " | buffer DXIL bytes: " << context.compiled_dxil_bytes()
                      << " | verified shader-processed RGBA8 pixels: "
                      << (SUCCEEDED(hr) ? 16384 : 0)
                      << " | interleaved buffer outputs: "
                      << (SUCCEEDED(hr) ? 1024 : 0)
                      << " | GPU submissions: " << context.submitted_commands()
                      << " | completed fence: " << context.completed_fence()
                      << " | HRESULT: 0x" << std::hex << static_cast<uint32_t>(hr)
                      << std::dec << "\n";
            if (!context.shader_diagnostics().empty())
                std::cerr << "DXC: " << context.shader_diagnostics() << "\n";
            std::cout << "Generated HLSL and textures only; no Xbox title shaders executed.\n";
            return SUCCEEDED(hr) ? 0 : 3;
        }
        if (verb == L"gpu-compute") {
            if (argc != 2) { usage(); return 2; }
            scarlett::gpu::DeviceContext context;
            HRESULT hr = context.initialize();
            if (SUCCEEDED(hr)) hr = context.compute_dispatch_test(0x13579BDFu);
            if (SUCCEEDED(hr)) hr = context.texture_roundtrip_test();
            if (SUCCEEDED(hr)) hr = context.compute_dispatch_test(0x2468ACE0u);
            std::wcout << L"ScarlettVI | real D3D12 DXIL compute dispatch: "
                       << (SUCCEEDED(hr) ? L"PASS" : L"FAIL") << L"\n";
            std::wcout << L"GPU: " << context.adapter_name() << L"\n";
            std::cout << "Shader model: 6.0 | compiled DXIL bytes: "
                      << context.compiled_dxil_bytes()
                      << " | verified compute outputs: "
                      << (SUCCEEDED(hr) ? 2048 : 0)
                      << " | interleaved texture pixels: "
                      << (SUCCEEDED(hr) ? 8192 : 0)
                      << " | GPU submissions: " << context.submitted_commands()
                      << " | completed fence: " << context.completed_fence()
                      << " | HRESULT: 0x" << std::hex << static_cast<uint32_t>(hr)
                      << std::dec << "\n";
            if (!context.shader_diagnostics().empty())
                std::cerr << "DXC: " << context.shader_diagnostics() << "\n";
            std::cout << "Generated HLSL only; no Xbox shaders or title code executed.\n";
            return SUCCEEDED(hr) ? 0 : 3;
        }
        if (verb == L"gpu-transfer") {
            if (argc != 2) { usage(); return 2; }
            scarlett::gpu::DeviceContext context;
            HRESULT hr = context.initialize();
            if (SUCCEEDED(hr)) hr = context.texture_roundtrip_test();
            if (SUCCEEDED(hr)) hr = context.texture_roundtrip_test();
            std::wcout << L"ScarlettVI | Windows D3D12 texture transfer: "
                       << (SUCCEEDED(hr) ? L"PASS" : L"FAIL") << L"\n";
            std::wcout << L"GPU: " << context.adapter_name() << L"\n";
            std::wcout << L"Texture: 128x64 RGBA8 | generated pixels: 8192"
                       << L" | queued submissions: " << context.submitted_commands()
                       << L" | completed fence: " << context.completed_fence()
                       << L" | HRESULT: 0x" << std::hex << static_cast<uint32_t>(hr)
                       << std::dec << L"\n";
            std::wcout << L"No Xbox shader or game instructions executed.\n";
            return SUCCEEDED(hr) ? 0 : 3;
        }
        if (verb == L"memory") {
            if (argc != 2) { usage(); return 2; }
            constexpr size_t page_bytes = scarlett::memory::PhysicalMemory::page_bytes;
            uintptr_t pages[2] = {};
            uintptr_t count = 2;
            const bool allocated = scarlett::xmem::XMemAllocatePhysicalPages(
                MEM_64K_PAGES, &count, pages) && count == 2;
            if (!allocated) {
                std::cerr << "64KiB page allocation failed, Win32 error: " << GetLastError() << "\n";
                return 3;
            }
            void* region = scarlett::xmem::XMemVirtualAlloc(
                nullptr, 3 * page_bytes, MEM_RESERVE | MEM_64K_PAGES, 0, PAGE_READWRITE);
            if (!region) {
                std::cerr << "Physical page placeholder reservation failed: " << GetLastError() << "\n";
                scarlett::xmem::XMemFreePhysicalPages(2, pages);
                return 3;
            }
            bool ok = scarlett::xmem::XMemMapPhysicalPages(region, 2, pages) == region;
            auto* alias = static_cast<uint8_t*>(region) + 2 * page_bytes;
            ok = ok && scarlett::xmem::XMemMapPhysicalPages(alias, 1, pages) == alias;
            if (ok) {
                auto* bytes = static_cast<volatile uint8_t*>(region);
                auto* second_view = static_cast<volatile uint8_t*>(static_cast<void*>(alias));
                bytes[0] = 0xAB;
                ok = second_view[0] == 0xAB; // true host memory alias, not a copy
                bytes[0] = 0;
                scarlett::xmem::VirtualQueryInfo query{};
                const bool query_ok = scarlett::xmem::XMemVirtualQuery(
                    GetCurrentProcess(), alias, &query) != 0;
                const bool attributes_ok = query_ok &&
                    query.basic.State == MEM_COMMIT &&
                    (query.basic.Protect & PAGE_READWRITE) == PAGE_READWRITE &&
                    (query.desktop_access_flags & 3) == 3;
                ok = ok && attributes_ok;
            }
            // The query bridge must distinguish read-only pages from
            // genuinely inaccessible pages; returning all-access for every
            // committed range would fool the Xbox BVH library's predicate.
            void* probe = VirtualAlloc(nullptr, page_bytes,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            bool query_protection_ok = probe != nullptr;
            if (probe) {
                DWORD original_protection = 0;
                scarlett::xmem::VirtualQueryInfo info{};
                const bool readonly = VirtualProtect(probe, page_bytes,
                                                     PAGE_READONLY, &original_protection) != 0;
                const bool readonly_query = readonly &&
                    scarlett::xmem::XMemVirtualQuery(GetCurrentProcess(), probe, &info) &&
                    info.basic.State == MEM_COMMIT &&
                    (info.desktop_access_flags & 3) == 1;
                DWORD previous_protection = 0;
                const bool inaccessible = VirtualProtect(probe, page_bytes,
                                                          PAGE_NOACCESS, &previous_protection) != 0;
                const bool inaccessible_query = inaccessible &&
                    scarlett::xmem::XMemVirtualQuery(GetCurrentProcess(), probe, &info) &&
                    info.basic.State == MEM_COMMIT &&
                    info.desktop_access_flags == 0;
                query_protection_ok = readonly_query && inaccessible_query;
                VirtualFree(probe, 0, MEM_RELEASE);
            }
            ok = ok && query_protection_ok;
            const bool unmapped_primary =
                scarlett::xmem::XMemMapPhysicalPages(region, 2, nullptr) == region;
            const bool unmapped_alias =
                scarlett::xmem::XMemMapPhysicalPages(alias, 1, nullptr) == alias;
            const bool freed_region =
                scarlett::xmem::VirtualFreeBridge(region, 0, MEM_RELEASE) != 0;
            const bool freed_pages = scarlett::xmem::XMemFreePhysicalPages(2, pages) != 0;
            ok = ok && unmapped_primary && unmapped_alias && freed_region && freed_pages;
            std::cout << "ScarlettVI | 64KiB physical-page aliasing check: "
                      << (ok ? "PASS" : "FAIL") << "\n"
                      << "Allocated pages: 2 | alias views: 3 | "
                      << "free/unmap: " << (freed_region && freed_pages ? "PASS" : "FAIL")
                      << " | remaining pages: "
                      << scarlett::memory::PhysicalMemory::instance().allocated_page_count()
                      << "\n";
            return ok ? 0 : 3;
        }
        if (verb == L"gpu") {
            if (argc != 2) { usage(); return 2; }
            const auto adapters = scarlett::gpu::probe_host_adapters();
            std::wcout << L"ScarlettVI | native Windows D3D12 backend probe\n";
            std::wcout << L"Adapters detected: " << adapters.size() << L"\n";
            bool working = false;
            for (const auto& adapter : adapters) {
                std::wcout << L"  GPU: " << adapter.adapter_name
                           << L" | VID: 0x" << std::hex << adapter.vendor_id
                           << L" DID: 0x" << adapter.device_id << std::dec
                           << L" | VRAM: " << (adapter.dedicated_video_memory_bytes / 1048576)
                           << L" MiB | FL12_0: " << (adapter.level_12_0 ? L"yes" : L"no") << L"\n";
                std::cout << "    D3D12 device: " << adapter.device_created
                          << " | queue: " << adapter.queue_created
                          << " | command list: " << adapter.command_list_created
                          << " | fence: " << adapter.fence_created
                          << " | upload buffer: " << adapter.upload_buffer_created
                          << " | root signature: " << adapter.root_signature_created
                          << " | nodes: " << adapter.node_count
                          << " | HRESULT: 0x" << std::hex
                          << static_cast<uint32_t>(adapter.result) << std::dec << "\n";
                working |= adapter.level_12_0 && adapter.command_list_created &&
                           adapter.fence_created && adapter.upload_buffer_created &&
                           adapter.root_signature_created;
            }
            std::cout << "Note: this tests Windows GPU capabilities, NOT Xbox D3D12_XS compatibility.\n";
            return working ? 0 : 3;
        }
        if (argc < 3) { usage(); return 2; }
        if (verb == L"archives") {
            if (argc != 3) { usage(); return 2; }
            const fs::path folder = fs::absolute(argv[2]);
            const auto archives = scarlett::formats::enumerate_rpf9(folder);
            std::cout << "ScarlettVI | RPF9 metadata inventory (no data extraction)\n"
                      << "Archive count: " << archives.size() << "\n";
            uint64_t total_bytes = 0;
            uint64_t total_entries = 0;
            for (const auto& archive : archives) {
                total_bytes += archive.archive_bytes;
                total_entries += archive.entry_count;
                std::cout << "  " << utf8(archive.path.filename().wstring())
                          << " | size: " << std::fixed << std::setprecision(1)
                          << (double(archive.archive_bytes) / (1024.0 * 1024.0)) << " MiB"
                          << " | TOC end: 0x" << std::hex << archive.toc_end
                          << " | index variant: 0x" << archive.index_variant << std::dec
                          << " | header entries: " << archive.entry_count << "\n";
            }
            std::cout << "Total archive bytes: " << total_bytes
                      << " | sum of header entry counts: " << total_entries
                      << "\nRead only: no content or decryption material accessed.\n";
            return 0;
        }
        if (verb != L"inspect" && verb != L"map" && verb != L"readiness") {
            usage();
            return 2;
        }
        const fs::path title = fs::absolute(argv[2]);
        const scarlett::PeFile pe(title);
        const auto& d = pe.details();
        std::map<std::string, size_t> counts;
        size_t total_symbols = 0;
        for (const auto& module : d.imports) {
            counts[scarlett::route_name(scarlett::classify_import(module.name, title.parent_path()))]++;
            total_symbols += module.symbols.size();
        }
        std::cout << "ScarlettVI | PE32+ x64 compatibility analysis\n"
                  << "Title: " << utf8(title.filename().wstring()) << "\n"
                  << "Image: " << (d.image_size / 1048576) << " MiB, "
                  << d.sections.size() << " sections, " << d.imports.size()
                  << " imported modules, " << total_symbols << " functions\n"
                  << "Entry RVA: 0x" << std::hex << d.entry_rva << std::dec
                  << " | TLS: " << (d.tls_rva ? "yes" : "no")
                  << " | x64 exception directory: " << (d.exception_rva ? "yes" : "no") << "\n";
        for (const auto& [kind, count] : counts)
            std::cout << "  " << kind << ": " << count << " modules\n";
        if (verb != L"readiness") {
            std::cout << "\nImport routing:\n";
            for (const auto& module : d.imports) {
                std::cout << "  [" << scarlett::route_name(scarlett::classify_import(module.name, title.parent_path())) << "] "
                          << module.name << " (" << module.symbols.size() << " imports)"
                          << (module.delay_loaded ? " [delay]" : "") << "\n";
            }
        }
        std::optional<scarlett::StartupReadiness> readiness;
        if (verb == L"map" || verb == L"readiness") {
            std::cout << "\nMapping PE image without executing code...\n";
            scarlett::MappedImage image(pe);
            std::cout << "Mapped at: 0x" << std::hex << image.base_address() << std::dec
                      << " | applied DIR64 relocations: " << image.applied_relocations() << "\n";
            scarlett::ImportRouter router;
            const auto binding = router.bind_windows(pe, image, title.parent_path());
            std::cout << "Windows imports patched: " << binding.host_symbols_bound << "/"
                      << binding.host_symbols_seen << " | system modules loaded: "
                      << binding.host_modules_loaded << "\n";
            scarlett::TitleModules local(title.parent_path(), pe);
            const auto local_report = local.bind(pe, image, router);
            const auto finalized = image.finalize_image();
            std::cout << "Mapped image sections protected: "
                      << (local_report.protected_sections + finalized.sections_protected)
                      << " | x64 unwind functions registered: "
                      << (local_report.registered_unwind_functions + finalized.unwind_functions_registered)
                      << "\n";
            std::cout << "TLS slots prepared for current thread: "
                      << (local_report.tls_slots_prepared + finalized.tls_slots_prepared)
                      << " | template bytes: "
                      << (local_report.tls_template_bytes + finalized.tls_template_bytes)
                      << " | callbacks deferred: "
                      << (local_report.deferred_tls_callbacks + finalized.deferred_tls_callbacks)
                      << "\n";
            std::cout << "Local DLL images mapped: " << local_report.title_images_mapped
                      << " | title import slots linked: " << local_report.local_imports_bound
                      << "/" << local_report.local_imports_seen << "\n";
            std::cout << "Nested Windows imports patched: " << local_report.nested_host_bound
                      << "/" << local_report.nested_host_seen << "\n";
            std::cout << "VirtualFree placeholder-aware import hooks: "
                      << (binding.memory_release_hooks_bound + local_report.nested_release_hooks_bound)
                      << "\n";
            std::cout << "Experimental XMem exports linked: "
                      << (binding.experimental_hle_symbols_bound + local_report.nested_experimental_hle_bound)
                      << " (partial ABI support only)\n";
            std::cout << "D3D12 root-signature serializer exports linked: "
                      << (binding.experimental_d3d12_exports_bound + local_report.nested_d3d12_exports_bound)
                      << " (desktop serializer ABI)\n";
            std::cout << "Experimental Xbox Device11 factory links: "
                      << (binding.experimental_d3d12_device_bound + local_report.nested_d3d12_device_bound)
                      << " (native COM interface only; Xbox-only features unsupported)\n";
            std::cout << "Experimental Xbox GPU VA configuration links: "
                      << (binding.experimental_gpu_va_config_bound + local_report.nested_gpu_va_config_bound)
                      << " (real sparse host GPU VA, Xbox tile mapping unsupported)\n";
            std::cout << "Xbox HLE imports pending: "
                      << (binding.xbox_symbols_pending + local_report.nested_xbox_pending)
                      << " | unknown nested imports: " << local_report.nested_unknown_pending << "\n";
            if (!local_report.missing_local_exports.empty()) {
                std::cout << "Unresolved local imports (" << local_report.missing_local_exports.size() << "):\n";
                for (size_t i = 0; i < std::min<size_t>(20, local_report.missing_local_exports.size()); ++i)
                    std::cout << "  " << local_report.missing_local_exports[i] << "\n";
            }
            if (!local_report.missing_nested_host.empty()) {
                std::cout << "Unresolved nested Windows imports (" << local_report.missing_nested_host.size() << "):\n";
                for (size_t i = 0; i < std::min<size_t>(20, local_report.missing_nested_host.size()); ++i)
                    std::cout << "  " << local_report.missing_nested_host[i] << "\n";
            }
            if (!binding.missing_host_symbols.empty()) {
                std::cout << "Unresolved Windows functions (" << binding.missing_host_symbols.size() << "):\n";
                for (size_t i = 0; i < std::min<size_t>(30, binding.missing_host_symbols.size()); ++i)
                    std::cout << "  " << binding.missing_host_symbols[i] << "\n";
            }
            std::cout << "Title entry point, TLS callbacks and game DLL code were NOT executed.\n";
            if (verb == L"readiness") {
                readiness = scarlett::assess_startup_readiness(pe, binding, local_report, finalized);
                scarlett::print_startup_readiness(*readiness, std::cout);
            }
        }
        if (argc > 3) {
            if (argc != 5 || std::wstring_view(argv[3]) != L"--json")
                throw std::runtime_error("Expected only --json <output.json>");
            const fs::path report(argv[4]);
            if (readiness)
                scarlett::write_startup_readiness_json(*readiness, report);
            else
                write_json(report, d, title.parent_path());
            std::cout << "Wrote report: " << utf8(report.wstring()) << "\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ScarlettVI error: " << error.what() << "\n";
        return 1;
    }
}
