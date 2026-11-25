#include "import_router.h"
#include "../loader/image_mapper.h"
#include "xmem_bridge.h"
#include "../audio/apu_memory.h"
#include "../gpu/xbox_device_factory.h"
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

const char* pending_kind_name(PendingKind kind) noexcept {
    switch (kind) {
    case PendingKind::XboxHle: return "xbox-hle";
    case PendingKind::UnknownModule: return "unknown-module";
    case PendingKind::WindowsExport: return "missing-host-export";
    case PendingKind::TitleExport: return "missing-title-export";
    }
    return "unknown";
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

FARPROC ImportRouter::resolve_system_export(const std::string& dll, const ImportedSymbol& symbol) {
    if (classify_import(dll, {}) != ImportRoute::Windows)
        return nullptr;
    HMODULE module = system_module(dll);
    if (!module) return nullptr;
    return symbol.by_ordinal ? GetProcAddress(module, MAKEINTRESOURCEA(symbol.ordinal))
                             : GetProcAddress(module, symbol.name.c_str());
}

BindingResult ImportRouter::bind_windows(const PeFile& pe, MappedImage& image,
                                        const std::filesystem::path& title_directory) {
    BindingResult result;
    const auto owner = pe.path().filename().string();
    auto add_pending = [&](const ImportedModule& module,
                           const ImportedSymbol& symbol, PendingKind kind) {
        result.pending.push_back(PendingImport{
            owner, module.name,
            symbol.by_ordinal ? "#" + std::to_string(symbol.ordinal) : symbol.name,
            symbol.iat_rva, kind, module.delay_loaded
        });
    };
    for (const auto& imported : pe.details().imports) {
        const auto route = classify_import(imported.name, title_directory);
        if (route != ImportRoute::Windows) {
            if (route == ImportRoute::XboxHle && lower(imported.name) == "xmem.dll") {
                for (const auto& symbol : imported.symbols) {
                    FARPROC address = symbol.by_ordinal ? nullptr : xmem::find_export(symbol.name);
                    if (address) {
                        image.patch_iat(symbol.iat_rva, reinterpret_cast<uint64_t>(address));
                        ++result.experimental_hle_symbols_bound;
                        if (symbol.name == "XMemGetWorkingSetStatistics")
                            ++result.experimental_xmem_error_only_bound;
                    } else {
                        ++result.xbox_symbols_pending;
                        add_pending(imported, symbol, PendingKind::XboxHle);
                    }
                }
            } else if (route == ImportRoute::XboxHle &&
                       lower(imported.name) == "acphal.dll") {
                // Bind verified host memory/SHAPE functions and a partial
                // HAL factory separately. Factory allocates real 0x1000
                // CPU buffers, but guest vtable +0x10 deliberately returns
                // E_NOTIMPL: no verified DSP configuration, audio rendering,
                // or complete guest object method ABI exists yet.
                for (const auto& symbol : imported.symbols) {
                    FARPROC address = symbol.by_ordinal
                        ? nullptr : audio::find_acphal_export(symbol.name);
                    if (address) {
                        image.patch_iat(symbol.iat_rva,
                                        reinterpret_cast<uint64_t>(address));
                        if (symbol.name == "AcpHalCreate")
                            ++result.experimental_apu_hal_partial_bound;
                        else
                            ++result.experimental_apu_host_alloc_bound;
                    } else {
                        ++result.xbox_symbols_pending;
                        add_pending(imported, symbol, PendingKind::XboxHle);
                    }
                }
            } else if (route == ImportRoute::XboxHle && lower(imported.name) == "d3d12_xs.dll") {
                // The desktop serializer has the same public ABI. We resolve it
                // from the Windows module, not from a proprietary Xbox DLL.
                // The Xbox device creation call uses the public
                // ID3D12Device11 IID. An independent, limited bridge returns
                // a real native Windows COM device for the observed version
                // and ring-size configuration. Xbox-only features and virtual
                // memory configuration remain unimplemented.
                for (const auto& symbol : imported.symbols) {
                    FARPROC address = nullptr;
                    if (!symbol.by_ordinal && symbol.name == "D3D12SerializeVersionedRootSignature")
                        address = resolve_system_export("d3d12.dll", symbol);
                    else if (!symbol.by_ordinal && symbol.name == "D3D12XboxCreateDevice")
                        address = reinterpret_cast<FARPROC>(&gpu::xbox_create_device_host);
                    else if (!symbol.by_ordinal && symbol.name == "D3D12ConfigureVirtualMemoryX")
                        address = reinterpret_cast<FARPROC>(&gpu::xbox_configure_virtual_memory_host);
                    if (address) {
                        image.patch_iat(symbol.iat_rva, reinterpret_cast<uint64_t>(address));
                        if (symbol.name == "D3D12XboxCreateDevice")
                            ++result.experimental_d3d12_device_bound;
                        else if (symbol.name == "D3D12ConfigureVirtualMemoryX")
                            ++result.experimental_gpu_va_config_bound;
                        else
                            ++result.experimental_d3d12_exports_bound;
                    } else {
                        ++result.xbox_symbols_pending;
                        add_pending(imported, symbol, PendingKind::XboxHle);
                    }
                }
            } else {
                switch (route) {
                case ImportRoute::TitleLocal: result.title_symbols_pending += imported.symbols.size(); break;
                case ImportRoute::XboxHle: result.xbox_symbols_pending += imported.symbols.size(); break;
                case ImportRoute::Unresolved: result.unknown_symbols_pending += imported.symbols.size(); break;
                default: break;
                }
                if (route == ImportRoute::XboxHle || route == ImportRoute::Unresolved)
                    for (const auto& symbol : imported.symbols)
                        add_pending(imported, symbol,
                            route == ImportRoute::XboxHle
                                ? PendingKind::XboxHle : PendingKind::UnknownModule);
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
                if (!symbol.by_ordinal && symbol.name == "VirtualFree") {
                    // The title's XMemVirtualAlloc can reserve 64 KiB placeholder
                    // regions; normal Win32 VirtualFree cannot release an entire
                    // split/aliased region. Intercept only this well-known host
                    // API, forwarding all other addresses to Windows untouched.
                    address = reinterpret_cast<FARPROC>(&xmem::VirtualFreeBridge);
                    ++result.memory_release_hooks_bound;
                }
                image.patch_iat(symbol.iat_rva, reinterpret_cast<uint64_t>(address));
                ++result.host_symbols_bound;
            } else {
                const auto identifier = symbol.by_ordinal ? "#" + std::to_string(symbol.ordinal) : symbol.name;
                result.missing_host_symbols.push_back(imported.name + "!" + identifier);
                add_pending(imported, symbol, PendingKind::WindowsExport);
            }
        }
    }
    for (const auto& [name, module] : modules_)
        if (module) ++result.host_modules_loaded;
    return result;
}
} // namespace scarlett
