#include "pe/pe_file.h"
#include "loader/image_mapper.h"
#include "runtime/import_router.h"
#include "loader/title_modules.h"
#include "formats/rpf9_header.h"
#include "gpu/host_backend.h"
#include "gpu/device_context.h"
#include "runtime/physical_memory.h"
#include "runtime/xmem_bridge.h"
#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
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
               << L"  ScarlettVI.exe map <title.exe> [--json <report.json>]\n"
               << L"  ScarlettVI.exe archives <title-directory-or-archive.rpf>\n"
               << L"  ScarlettVI.exe gpu\n"
               << L"  ScarlettVI.exe gpu-transfer\n"
               << L"  ScarlettVI.exe memory\n"
               << L"\nmap is a data-only PE memory mapping/import probe; it DOES NOT execute title code.\n";
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { usage(); return 0; }
    try {
        const std::wstring verb = argv[1];
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
            }
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
        if (verb != L"inspect" && verb != L"map") {
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
        std::cout << "\nImport routing:\n";
        for (const auto& module : d.imports) {
            std::cout << "  [" << scarlett::route_name(scarlett::classify_import(module.name, title.parent_path())) << "] "
                      << module.name << " (" << module.symbols.size() << " imports)"
                      << (module.delay_loaded ? " [delay]" : "") << "\n";
        }
        if (verb == L"map") {
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
                      << " (desktop ABI bridge; GPU device HLE not implemented)\n";
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
        }
        if (argc > 3) {
            if (argc != 5 || std::wstring_view(argv[3]) != L"--json")
                throw std::runtime_error("Expected only --json <output.json>");
            const fs::path report(argv[4]);
            write_json(report, d, title.parent_path());
            std::cout << "Wrote report: " << utf8(report.wstring()) << "\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ScarlettVI error: " << error.what() << "\n";
        return 1;
    }
}
