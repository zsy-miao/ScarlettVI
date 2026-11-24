#include "title_modules.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
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
}

TitleModules::TitleModules(std::filesystem::path title_directory, const PeFile& executable)
    : directory_(std::move(title_directory)) {
    find_dependencies(executable);
}

void TitleModules::find_dependencies(const PeFile& pe) {
    for (const auto& imp : pe.details().imports) {
        if (classify_import(imp.name, directory_) != ImportRoute::TitleLocal)
            continue;
        const std::string key = lower(imp.name);
        if (modules_.contains(key)) continue;
        const std::filesystem::path path = directory_ / imp.name;
        auto loaded = std::make_unique<Record>(path);
        const PeFile* recurse = &loaded->pe;
        modules_.emplace(key, std::move(loaded)); // insert before recursion: handles cycles
        find_dependencies(*recurse);
    }
}

void TitleModules::bind_local(const PeFile& pe, MappedImage& image, TitleBindingReport& report) {
    for (const auto& module : pe.details().imports) {
        if (classify_import(module.name, directory_) != ImportRoute::TitleLocal)
            continue;
        const auto it = modules_.find(lower(module.name));
        if (it == modules_.end())
            throw std::runtime_error("Internal error: local dependency not mapped");
        const auto& target = *it->second;
        for (const auto& imp : module.symbols) {
            ++report.local_imports_seen;
            const auto found = std::find_if(target.pe.details().exports.begin(),
                target.pe.details().exports.end(), [&](const ExportedSymbol& exp) {
                    if (imp.by_ordinal) return exp.ordinal == imp.ordinal;
                    return !exp.name.empty() && exp.name == imp.name;
                });
            if (found != target.pe.details().exports.end() &&
                found->rva != 0 && found->forwarder.empty() &&
                found->rva < target.pe.details().image_size) {
                image.patch_iat(imp.iat_rva, target.image.base_address() + found->rva);
                ++report.local_imports_bound;
            } else {
                std::string detail = module.name + "!" + symbol_description(imp);
                if (found != target.pe.details().exports.end() && !found->forwarder.empty())
                    detail += " (forwarded to " + found->forwarder + ")";
                report.missing_local_exports.push_back(std::move(detail));
            }
        }
    }
}

TitleBindingReport TitleModules::bind(const PeFile& executable, MappedImage& executable_image,
                                      ImportRouter& router) {
    TitleBindingReport report;
    report.title_images_mapped = modules_.size();
    bind_local(executable, executable_image, report);
    for (auto& [name, module] : modules_) {
        bind_local(module->pe, module->image, report);
        const BindingResult binding = router.bind_windows(module->pe, module->image, directory_);
        report.nested_host_seen += binding.host_symbols_seen;
        report.nested_host_bound += binding.host_symbols_bound;
        report.nested_xbox_pending += binding.xbox_symbols_pending;
        report.nested_unknown_pending += binding.unknown_symbols_pending;
        report.missing_nested_host.insert(report.missing_nested_host.end(),
            binding.missing_host_symbols.begin(), binding.missing_host_symbols.end());
    }
    // No entry point, TLS callback, or DLL initializer has been executed.
    // Module memory remains live until all import reports are complete.
    return report;
}
} // namespace scarlett
