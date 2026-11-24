#pragma once
#include "../pe/pe_file.h"
#include "../runtime/import_router.h"
#include "image_mapper.h"
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace scarlett {
struct TitleBindingReport {
    size_t title_images_mapped = 0;
    size_t local_imports_seen = 0;
    size_t local_imports_bound = 0;
    size_t nested_host_seen = 0;
    size_t nested_host_bound = 0;
    size_t nested_release_hooks_bound = 0;
    size_t nested_xbox_pending = 0;
    size_t nested_experimental_hle_bound = 0;
    size_t nested_d3d12_exports_bound = 0;
    size_t nested_d3d12_device_bound = 0;
    size_t nested_gpu_va_config_bound = 0;
    size_t nested_unknown_pending = 0;
    size_t protected_sections = 0;
    size_t registered_unwind_functions = 0;
    size_t tls_slots_prepared = 0;
    size_t tls_template_bytes = 0;
    size_t deferred_tls_callbacks = 0;
    std::vector<std::string> missing_local_exports;
    std::vector<std::string> missing_nested_host;
    std::vector<PendingImport> pending;
};

// PE loader for the title's bundled DLLs, without LoadLibrary/DllMain/TLS.
// Resolves native exports to our independently mapped in-process images.
class TitleModules final {
public:
    TitleModules(std::filesystem::path title_directory, const PeFile& executable);
    TitleBindingReport bind(const PeFile& executable, MappedImage& executable_image,
                            ImportRouter& router);
private:
    struct Record {
        explicit Record(const std::filesystem::path& path) : pe(path), image(pe) {}
        PeFile pe;
        MappedImage image;
    };
    void find_dependencies(const PeFile& pe);
    void bind_local(const PeFile& pe, MappedImage& image, ImportRouter& router,
                    TitleBindingReport& report);
    uint64_t resolve_export(const std::string& module_name, const ImportedSymbol& imported,
                            ImportRouter& router, std::vector<std::string>& visited) const;
    std::filesystem::path directory_;
    std::map<std::string, std::unique_ptr<Record>> modules_;
};
} // namespace scarlett
