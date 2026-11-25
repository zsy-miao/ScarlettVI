#include "guest_gpu_page_registry.h"
#include <algorithm>
#include <array>
#include <limits>
#include <tuple>
#include <vector>

namespace scarlett::gpu {
namespace {
constexpr uint64_t tile = GpuPageStager::page_bytes;
struct LocatedPage {
    GuestGpuPageRegistry::PageBinding value{};
    ID3D12Resource* source_resource = nullptr;
    ID3D12Resource* alias_resource = nullptr;
};
}

// Pure host plan: no GPU submissions, XMem page pins or resource ownership
// changes. Sort by guest source VA and group only when both GPU alias slots
// advance one page inside the same two native reserved D3D12 resources.
HRESULT GuestGpuPageRegistry::plan_scattered(
    std::span<const PageBinding> pages, ScatteredPlan& output) const noexcept {
    if (pages.empty() || pages.size() > heap_capacity_tiles)
        return E_INVALIDARG;
    std::vector<LocatedPage> ordered;
    ScatteredPlan prospective;
    try {
        ordered.reserve(pages.size());
        for (const auto& p : pages) {
            const auto valid_alias = [](const void* ptr) noexcept {
                return ptr && !(reinterpret_cast<uintptr_t>(ptr) & (tile - 1));
            };
            if (!p.frame || !valid_alias(p.first_cpu_alias) ||
                !valid_alias(p.second_cpu_alias) ||
                p.first_cpu_alias == p.second_cpu_alias ||
                p.source.offset % tile || p.alias.offset % tile ||
                p.source == p.alias)
                return E_INVALIDARG;
            uint64_t src_local = 0, dst_local = 0;
            ID3D12Resource* src = windows_.tile_resource(
                p.source.window, p.source.offset, &src_local);
            ID3D12Resource* dst = windows_.tile_resource(
                p.alias.window, p.alias.offset, &dst_local);
            if (!src || !dst || src == dst) return E_INVALIDARG;
            for (const auto& earlier : ordered) {
                if (earlier.value.frame == p.frame ||
                    earlier.value.source == p.source ||
                    earlier.value.source == p.alias ||
                    earlier.value.alias == p.source ||
                    earlier.value.alias == p.alias)
                    return E_INVALIDARG;
            }
            ordered.push_back({p, src, dst});
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const LocatedPage& a, const LocatedPage& b) {
            return std::tie(a.value.source.window, a.value.source.offset,
                            a.value.alias.window, a.value.alias.offset) <
                   std::tie(b.value.source.window, b.value.source.offset,
                            b.value.alias.window, b.value.alias.offset);
        });

        prospective.runs.reserve(ordered.size());
        for (size_t i = 0; i < ordered.size(); ++i) {
            bool extend = false;
            if (i) {
                const auto& prev = ordered[i-1];
                const auto& current = ordered[i];
                extend =
                    prev.source_resource == current.source_resource &&
                    prev.alias_resource == current.alias_resource &&
                    prev.value.source.window == current.value.source.window &&
                    prev.value.alias.window == current.value.alias.window &&
                    prev.value.source.offset <= UINT64_MAX - tile &&
                    prev.value.alias.offset <= UINT64_MAX - tile &&
                    current.value.source.offset == prev.value.source.offset + tile &&
                    current.value.alias.offset == prev.value.alias.offset + tile;
            }
            if (!extend) prospective.runs.emplace_back();
            prospective.runs.back().push_back(ordered[i].value);
        }
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    output = std::move(prospective);
    return S_OK;
}

void GuestGpuPageRegistry::inject_scattered_run_failure_for_test(
    size_t run_index) noexcept {
    std::lock_guard lock(mutex_);
    test_fail_scattered_run_ = run_index;
}

// Keep an OUTER CPU pin on every requested frame throughout all per-run
// registrations. Each successful bind_batch adds its own lifetime pin and
// continues holding it after the outer pins are dropped. If a later native
// mapping fails, retire only this operation's runs; NEVER clear unrelated
// pre-existing registry entries. If retirement itself fails, internal pins
// and D3D12 heap slots stay live for a later caller retry.
HRESULT GuestGpuPageRegistry::bind_scattered(
    std::span<const PageBinding> pages) noexcept {
    ScatteredPlan plan;
    HRESULT hr = plan_scattered(pages, plan);
    if (FAILED(hr)) return hr;

    {
        std::lock_guard lock(mutex_);
        if (bindings_.size() + pages.size() > heap_capacity_tiles)
            return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
        for (const auto& run : plan.runs)
            for (const auto& p : run) {
                if (bindings_.contains(p.frame))
                    return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
                if (gpu_slots_.contains(p.source) ||
                    gpu_slots_.contains(p.alias))
                    return HRESULT_FROM_WIN32(ERROR_BUSY);
            }
    }

    size_t pinned = 0;
    auto release_outer = [this, &pages, &pinned]() noexcept {
        for (size_t i = pinned; i > 0; --i) {
            const auto& p = pages[i-1];
            physical_.unpin_mapped_alias(p.second_cpu_alias, p.frame);
            physical_.unpin_mapped_alias(p.first_cpu_alias, p.frame);
        }
        pinned = 0;
    };
    for (const auto& p : pages) {
        if (!physical_.pin_mapped_alias(p.first_cpu_alias, p.frame)) {
            const DWORD error = GetLastError();
            release_outer();
            return HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_ADDRESS);
        }
        if (!physical_.pin_mapped_alias(p.second_cpu_alias, p.frame)) {
            const DWORD error = GetLastError();
            physical_.unpin_mapped_alias(p.first_cpu_alias, p.frame);
            release_outer();
            return HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_ADDRESS);
        }
        ++pinned;
    }

    std::optional<size_t> injected_run;
    {
        std::lock_guard lock(mutex_);
        injected_run = test_fail_scattered_run_;
        test_fail_scattered_run_.reset();
    }
    for (size_t run_i = 0; run_i < plan.runs.size(); ++run_i) {
        if (injected_run && run_i == *injected_run)
            inject_next_mapping_wait_failure_for_test();
        hr = bind_batch(plan.runs[run_i]);
        if (SUCCEEDED(hr)) continue;

        HRESULT rollback_error = S_OK;
        // This includes the failing run if GPU calls had already been
        // submitted; the created entries may need fence-confirmed unmap.
        for (size_t reverse = run_i + 1; reverse > 0; --reverse) {
            const auto& run = plan.runs[reverse - 1];
            std::array<uintptr_t, heap_capacity_tiles> live{};
            size_t count = 0;
            for (const auto& p : run)
                if (heap_tile_for(p.frame).has_value())
                    live[count++] = p.frame;
            if (!count) continue;
            HRESULT retired = E_FAIL;
            if (count == run.size())
                retired = unbind_batch(std::span<const uintptr_t>(
                    live.data(), count));
            else {
                retired = S_OK;
                for (size_t j = 0; j < count; ++j) {
                    const HRESULT single = unbind(live[j]);
                    if (FAILED(single)) retired = single;
                }
            }
            if (FAILED(retired) && SUCCEEDED(rollback_error))
                rollback_error = retired;
        }
        release_outer();
        return FAILED(rollback_error) ? rollback_error : hr;
    }
    release_outer();
    return S_OK;
}

// Plan from the known live entries; caller provides ONLY currently bound
// frames. If an unmap fails, already completed groups stay retired, and the
// failed group and all later groups remain owned/pinned. Retrying requires
// the still-live subset (safe to discover with heap_tile_for).
HRESULT GuestGpuPageRegistry::unbind_scattered(
    std::span<const uintptr_t> frames) noexcept {
    if (frames.empty() || frames.size() > heap_capacity_tiles)
        return E_INVALIDARG;
    std::vector<PageBinding> pages;
    try {
        pages.reserve(frames.size());
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < frames.size(); ++i) {
            const auto it = bindings_.find(frames[i]);
            if (it == bindings_.end())
                return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            for (size_t j = 0; j < i; ++j)
                if (frames[j] == frames[i]) return E_INVALIDARG;
            const Binding& entry = it->second;
            pages.push_back({
                frames[i], entry.first_cpu, entry.second_cpu,
                entry.source, entry.alias
            });
        }
    } catch (...) {
        return E_OUTOFMEMORY;
    }

    ScatteredPlan plan;
    HRESULT hr = plan_scattered(pages, plan);
    if (FAILED(hr)) return hr;
    for (const auto& run : plan.runs) {
        std::array<uintptr_t, heap_capacity_tiles> members{};
        for (size_t i = 0; i < run.size(); ++i) members[i] = run[i].frame;
        hr = unbind_batch(std::span<const uintptr_t>(
            members.data(), run.size()));
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}
} // namespace scarlett::gpu
