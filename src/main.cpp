#include "pe/pe_file.h"
#include "loader/image_mapper.h"
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
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
}
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
enum class Origin { Host, Title, Xbox, Unresolved };
std::string origin_name(Origin kind) {
    switch (kind) {
    case Origin::Host: return "windows";
    case Origin::Title: return "title-local";
    case Origin::Xbox: return "xbox-hle-required";
    default: return "unresolved";
    }
}
Origin classify(const std::string& dll, const fs::path& title_directory) {
    const std::string name = lower(dll);
    if (name.rfind("api-ms-win-", 0) == 0 || name.rfind("ext-ms-win-", 0) == 0)
        return Origin::Host;
    static const std::set<std::string> xbox = {
        "d3d12_xs.dll", "xg_xs.dll", "xmem.dll", "xapu.dll", "acphal.dll",
        "xcrdapi.dll", "chad.dll", "kernelx.dll", "xgameruntime.dll",
        "xgamert.dll", "xdpp.dll", "xsystem.dll"
    };
    if (xbox.contains(name)) return Origin::Xbox;
    static const std::set<std::string> host = {
        "kernel32.dll", "kernelbase.dll", "ntdll.dll", "user32.dll",
        "gdi32.dll", "ole32.dll", "oleaut32.dll", "combase.dll",
        "advapi32.dll", "bcrypt.dll", "crypt32.dll", "d3d12.dll",
        "dxgi.dll", "dxcompiler.dll", "gameinput.dll", "mfplat.dll",
        "mfreadwrite.dll", "winhttp.dll", "ws2_32.dll",
        "ucrtbase.dll", "vcruntime140.dll", "vcruntime140_1.dll",
        "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll"
    };
    if (host.contains(name)) return Origin::Host;
    std::error_code ec;
    if (fs::is_regular_file(title_directory / fs::path(dll), ec)) return Origin::Title;
    // Windows filename matching is case insensitive, and std::filesystem may not
    // share that behavior on every host. Do not execute any game DLL while probing.
    return Origin::Unresolved;
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
             << "\", \"route\": \"" << origin_name(classify(module.name, directory))
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
            counts[origin_name(classify(module.name, title.parent_path()))]++;
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
            std::cout << "  [" << origin_name(classify(module.name, title.parent_path())) << "] "
                      << module.name << " (" << module.symbols.size() << " imports)"
                      << (module.delay_loaded ? " [delay]" : "") << "\n";
        }
        if (verb == L"map") {
            std::cout << "\nMapping PE image without executing code...\n";
            const scarlett::MappedImage image(pe);
            std::cout << "Mapped at: 0x" << std::hex << image.base_address() << std::dec
                      << " | applied DIR64 relocations: " << image.applied_relocations() << "\n";
            std::cout << "Import binding and execution are intentionally separate steps.\n";
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
