#pragma once
#include "../pe/pe_file.h"
#include <Windows.h>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace scarlett {
class MappedImage;
enum class ImportRoute { Windows, TitleLocal, XboxHle, Unresolved };
ImportRoute classify_import(const std::string& dll_name, const std::filesystem::path& title_dir);
const char* route_name(ImportRoute route) noexcept;

enum class PendingKind { XboxHle, UnknownModule, WindowsExport, TitleExport };
const char* pending_kind_name(PendingKind kind) noexcept;
struct PendingImport {
    std::string owner;
    std::string module;
    std::string symbol;
    uint32_t iat_rva = 0;
    PendingKind kind = PendingKind::XboxHle;
    bool delay_loaded = false;
};
struct BindingResult {
    size_t host_symbols_seen = 0;
    size_t host_symbols_bound = 0;
    size_t memory_release_hooks_bound = 0;
    size_t host_modules_loaded = 0;
    size_t experimental_hle_symbols_bound = 0;
    size_t experimental_apu_host_alloc_bound = 0;
    size_t experimental_apu_hal_partial_bound = 0;
    size_t experimental_xapu_partial_bound = 0;
    size_t experimental_xmem_error_only_bound = 0;
    size_t experimental_d3d12_exports_bound = 0;
    size_t experimental_d3d12_device_bound = 0;
    size_t experimental_gpu_va_config_bound = 0;
    size_t xbox_symbols_pending = 0;
    size_t title_symbols_pending = 0;
    size_t unknown_symbols_pending = 0;
    std::vector<std::string> missing_host_symbols;
    std::vector<PendingImport> pending;
};

// Keeps Windows modules loaded for the lifetime of the mapped title. No target
// code is executed here: only host system DLL imports are bound.
class ImportRouter final {
public:
    ImportRouter() = default;
    ~ImportRouter();
    ImportRouter(const ImportRouter&) = delete;
    ImportRouter& operator=(const ImportRouter&) = delete;

    BindingResult bind_windows(const PeFile& pe, MappedImage& image,
                               const std::filesystem::path& title_directory);
    // Resolves a Windows API-set or system export for local PE export forwarding.
    // Loads only from Windows System32; never loads arbitrary game directory DLLs.
    FARPROC resolve_system_export(const std::string& dll, const ImportedSymbol& symbol);
private:
    HMODULE system_module(const std::string& name);
    std::map<std::string, HMODULE> modules_;
};
} // namespace scarlett
