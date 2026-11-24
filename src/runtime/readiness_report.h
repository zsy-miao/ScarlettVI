#pragma once
#include "../loader/title_modules.h"
#include "../loader/image_mapper.h"
#include "../runtime/import_router.h"
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace scarlett {
struct StartupReadiness {
    std::string executable;
    size_t local_dlls = 0;
    size_t windows_imports_bound = 0;
    size_t windows_imports_total = 0;
    size_t title_imports_bound = 0;
    size_t title_imports_total = 0;
    size_t xbox_experimental_memory_bindings = 0;
    size_t xbox_experimental_d3d12_bindings = 0;
    size_t registered_unwind_functions = 0;
    size_t tls_templates_prepared = 0;
    size_t tls_callbacks_deferred = 0;
    std::vector<PendingImport> pending;
    // These are independent, known process-initialization blockers even if
    // every imported symbol eventually has an address.
    std::vector<std::string> required_runtime_milestones;
    bool safe_to_execute_title = false;
};
StartupReadiness assess_startup_readiness(const PeFile& executable,
    const BindingResult& primary, const TitleBindingReport& nested,
    const ImageFinalizeReport& primary_finalize);
void print_startup_readiness(const StartupReadiness& readiness, std::ostream& output);
void write_startup_readiness_json(const StartupReadiness& readiness,
                                  const std::filesystem::path& output_path);
} // namespace scarlett
