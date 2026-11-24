#include "import_router.h"
#include "../loader/image_mapper.h"
#include "xmem_bridge.h"
#include <algorithm>
#include <set>
#include <string>
#include <system_error>

namespace scarlett {
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
std::wstring wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (!count) return {};
    std::wstring result(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            utf8.data(), static_cast<int>(utf8.size()), result.data(), count))
        return {};
    return result;
}
bool is_host_crt(const std::string& name) {
    return name.rfind("msvcp", 0) == 0 || name.rfind("vcruntime", 0) == 0 ||
           name == "concrt140.dll" || name == "vccorlib140.dll";
}
}

ImportRoute classify_import(const std::string& dll_name, const std::filesystem::path& title_dir) {
    const auto name = lower(dll_name);
    if (name.rfind("api-ms-win-", 0) == 0 || name.rfind("ext-ms-win-", 0) == 0)
        return ImportRoute::Windows;
    static const std::set<std::string> xbox = {
        "d3d12_xs.dll", "xg_xs.dll", "xmem.dll", "xapu.dll", "acphal.dll",
        "xcrdapi.dll", "chad.dll", "kernelx.dll", "xgameruntime.dll",
        "xgamert.dll", "xdpp.dll", "xsystem.dll"
    };
    if (xbox.contains(name)) return ImportRoute::XboxHle;
    // The title's own CRT binaries must not be accidentally replaced with a
    // different system CRT simply because Windows has the same DLL name.
    if (is_host_crt(name)) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(title_dir / dll_name, ec))
            return ImportRoute::TitleLocal;
    }
    static const std::set<std::string> windows = {
        "kernel32.dll", "kernelbase.dll", "ntdll.dll", "user32.dll",
        "gdi32.dll", "ole32.dll", "oleaut32.dll", "combase.dll",
        "advapi32.dll", "bcrypt.dll", "crypt32.dll", "d3d12.dll",
        "dxgi.dll", "dxcompiler.dll", "gameinput.dll", "mfplat.dll",
        "mfreadwrite.dll", "winhttp.dll", "ws2_32.dll",
        "ucrtbase.dll", "vcruntime140.dll", "vcruntime140_1.dll",
        "xaudio2_9.dll",
        "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll"
    };
    if (windows.contains(name)) return ImportRoute::Windows;
    std::error_code ec;
    if (std::filesystem::is_regular_file(title_dir / dll_name, ec))
        return ImportRoute::TitleLocal;
    return ImportRoute::Unresolved;
}

const char* route_name(ImportRoute route) noexcept {
    switch (route) {
    case ImportRoute::Windows: return "windows";
    case ImportRoute::TitleLocal: return "title-local";
    case ImportRoute::XboxHle: return "xbox-hle-required";
    default: return "unresolved";
    }
}

ImportRouter::~ImportRouter() {
    for (auto& [name, module] : modules_) {
        if (module) FreeLibrary(module);
    }
}

HMODULE ImportRouter::system_module(const std::string& name) {
    const auto key = lower(name);
    if (const auto found = modules_.find(key); found != modules_.end())
        return found->second;
    HMODULE module = nullptr;
    const auto module_name = wide(name);
    // LOAD_LIBRARY_SEARCH_SYSTEM32 excludes the game folder and arbitrary PATH
    // contents. No code from the title directory is loaded or executed.
    if (!module_name.empty())
        module = LoadLibraryExW(module_name.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    modules_.emplace(key, module);
    return module;
}

BindingResult ImportRouter::bind_windows(const PeFile& pe, MappedImage& image,
                                        const std::filesystem::path& title_directory) {
    BindingResult result;
    for (const auto& imported : pe.details().imports) {
        const auto route = classify_import(imported.name, title_directory);
        if (route != ImportRoute::Windows) {
            if (route == ImportRoute::XboxHle && lower(imported.name) == "xmem.dll") {
                for (const auto& symbol : imported.symbols) {
                    FARPROC address = symbol.by_ordinal ? nullptr : xmem::find_export(symbol.name);
                    if (address) {
                        image.patch_iat(symbol.iat_rva, reinterpret_cast<uint64_t>(address));
                        ++result.experimental_hle_symbols_bound;
                    } else {
                        ++result.xbox_symbols_pending;
                    }
                }
            } else {
                switch (route) {
                case ImportRoute::TitleLocal: result.title_symbols_pending += imported.symbols.size(); break;
                case ImportRoute::XboxHle: result.xbox_symbols_pending += imported.symbols.size(); break;
                case ImportRoute::Unresolved: result.unknown_symbols_pending += imported.symbols.size(); break;
                default: break;
                }
            }
            continue;
        }
        HMODULE module = system_module(imported.name);
        for (const auto& symbol : imported.symbols) {
            ++result.host_symbols_seen;
            FARPROC address = nullptr;
            if (module) {
                if (symbol.by_ordinal)
                    address = GetProcAddress(module, MAKEINTRESOURCEA(symbol.ordinal));
                else
                    address = GetProcAddress(module, symbol.name.c_str());
            }
            if (address) {
                image.patch_iat(symbol.iat_rva, reinterpret_cast<uint64_t>(address));
                ++result.host_symbols_bound;
            } else {
                const auto identifier = symbol.by_ordinal ? "#" + std::to_string(symbol.ordinal) : symbol.name;
                result.missing_host_symbols.push_back(imported.name + "!" + identifier);
            }
        }
    }
    for (const auto& [name, module] : modules_)
        if (module) ++result.host_modules_loaded;
    return result;
}
} // namespace scarlett
