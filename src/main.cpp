#include "pe/pe_file.h"
#include "loader/image_mapper.h"
#include "runtime/import_router.h"
#include "loader/title_modules.h"
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
               << L"\nmap is a data-only PE memory mapping/relocation probe; it DOES NOT execute title code.\n";
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { usage(); return argc == 1 ? 0 : 2; }
    try {
        const std::wstring verb = argv[1];
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
            std::cout << "Experimental XMem exports linked: "
                      << (binding.experimental_hle_symbols_bound + local_report.nested_experimental_hle_bound)
                      << " (partial ABI support only)\n";
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
