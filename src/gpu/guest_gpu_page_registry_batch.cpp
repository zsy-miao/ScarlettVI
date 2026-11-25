#include "guest_gpu_page_registry.h"
#include "fence_safety.h"
#include <algorithm>
#include <array>
#include <exception>
#include <vector>

namespace scarlett::gpu {

// Native host-only batch registration. Both guest GPU alias groups MUST each
// occupy consecutive tiles within one ID3D12Resource, not cross 4GiB native
// segments. All CPU VA pins and stager allocations are committed BEFORE any
// GPU UpdateTileMappings call; incomplete mappings are retained on GPU error.
HRESULT GuestGpuPageRegistry::bind_batch(
    std::span<const PageBinding> pages) noexcept {
    constexpr size_t tile = GpuPageStager::page_bytes;
    const size_t count = pages.size();
    if (!device_) return E_POINTER;
    if (!count || count > heap_capacity_tiles) return E_INVALIDARG;
    const uint64_t span_bytes = static_cast<uint64_t>(count - 1) * tile;
    const PageBinding& first = pages.front();
    if (first.source.offset > UINT64_MAX - span_bytes ||
        first.alias.offset > UINT64_MAX - span_bytes)
        return E_INVALIDARG;

    std::lock_guard lock(mutex_);
    const auto invalid_alias = [](const void* p) noexcept {
        return !p || (reinterpret_cast<uintptr_t>(p) &
                      (GpuPageStager::page_bytes - 1)) != 0;
    };
    const auto src = first.source;
    const auto dst = first.alias;
    uint64_t src_local = 0, dst_local = 0;
    ID3D12Resource* source_resource = windows_.tile_resource(
        src.window, src.offset, &src_local);
    ID3D12Resource* alias_resource = windows_.tile_resource(
        dst.window, dst.offset, &dst_local);
    if (!source_resource || !alias_resource ||
        source_resource == alias_resource) return E_INVALIDARG;
    // Every tile in the selected batch must be in each same native resource,
    // which catches 4GiB boundary crossings before pinning anything.
    for (size_t i = 0; i < count; ++i) {
        const auto& page = pages[i];
        if (!page.frame || invalid_alias(page.first_cpu_alias) ||
            invalid_alias(page.second_cpu_alias) ||
            page.first_cpu_alias == page.second_cpu_alias ||
            page.source.window != src.window ||
            page.alias.window != dst.window ||
            page.source.offset != src.offset + i * tile ||
            page.alias.offset != dst.offset + i * tile ||
            page.source == page.alias)
            return E_INVALIDARG;

        uint64_t a = 0, b = 0;
        if (windows_.tile_resource(src.window, page.source.offset, &a) !=
                source_resource ||
            windows_.tile_resource(dst.window, page.alias.offset, &b) !=
                alias_resource)
            return E_INVALIDARG;
        if (bindings_.contains(page.frame))
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        if (gpu_slots_.contains(page.source) ||
            gpu_slots_.contains(page.alias))
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        for (size_t j = 0; j < i; ++j)
            if (pages[j].frame == page.frame ||
                pages[j].source == page.alias ||
                pages[j].alias == page.source)
                return E_INVALIDARG;
    }

    std::array<bool, heap_capacity_tiles> prospective = occupied_heap_tiles_;
    std::vector<uint32_t> chosen;
    try {
        chosen.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto it = std::find(
                prospective.begin(), prospective.end(), false);
            if (it == prospective.end())
                return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
            const uint32_t index = static_cast<uint32_t>(
                it - prospective.begin());
            prospective[index] = true;
            chosen.push_back(index);
        }
    } catch (...) { return E_OUTOFMEMORY; }

    // Pin real CPU aliases for EVERY frame before publishing or enqueuing
    // native GPU mappings. Partial pin failures are undone pre-submission.
    size_t pinned = 0;
    const auto unpin_all = [this, &pages, &pinned]() noexcept {
        for (size_t i = pinned; i > 0; --i) {
            const auto& p = pages[i - 1];
            physical_.unpin_mapped_alias(p.second_cpu_alias, p.frame);
            physical_.unpin_mapped_alias(p.first_cpu_alias, p.frame);
        }
        pinned = 0;
    };
    for (const auto& page : pages) {
        if (!physical_.pin_mapped_alias(page.first_cpu_alias, page.frame)) {
            const DWORD error = GetLastError();
            unpin_all();
            return HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_ADDRESS);
        }
        if (!physical_.pin_mapped_alias(page.second_cpu_alias, page.frame)) {
            const DWORD error = GetLastError();
            physical_.unpin_mapped_alias(page.first_cpu_alias, page.frame);
            unpin_all();
            return HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_ADDRESS);
        }
        ++pinned;
    }

    const auto early_failure = [&](HRESULT error) noexcept {
        unpin_all();
        return error;
    };

    // Lazy shared GPU resources: still only one real command queue, heap,
    // batch fence and event for all registered physical frames.
    if (!queue_) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        const HRESULT hr = device_->CreateCommandQueue(
            &desc, IID_PPV_ARGS(queue_.GetAddressOf()));
        if (FAILED(hr)) return early_failure(hr);
    }
    if (!shared_tile_heap_) {
        D3D12_HEAP_DESC heap{};
        heap.SizeInBytes = uint64_t{heap_capacity_tiles} * tile;
        heap.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        heap.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        const HRESULT hr = device_->CreateHeap(
            &heap, IID_PPV_ARGS(shared_tile_heap_.GetAddressOf()));
        if (FAILED(hr)) return early_failure(hr);
    }
    if (!batch_fence_) {
        const HRESULT hr = device_->CreateFence(
            0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(batch_fence_.GetAddressOf()));
        if (FAILED(hr)) return early_failure(hr);
    }
    if (!batch_event_) {
        batch_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!batch_event_)
            return early_failure(HRESULT_FROM_WIN32(GetLastError()));
    }
    if (next_batch_fence_ == UINT64_MAX)
        return early_failure(HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW));

    std::vector<Binding> prepared;
    try { prepared.reserve(count); }
    catch (...) { return early_failure(E_OUTOFMEMORY); }
    for (size_t i = 0; i < count; ++i) {
        const auto& p = pages[i];
        Binding entry{};
        try {
            entry.stager = std::make_unique<GpuPageStager>();
        } catch (...) {
            prepared.clear(); // no tile mappings were queued yet
            return early_failure(E_OUTOFMEMORY);
        }
        const HRESULT hr = entry.stager->open(
            device_.Get(), windows_, p.source.window, p.source.offset,
            p.alias.window, p.alias.offset, queue_.Get(),
            shared_tile_heap_.Get(), chosen[i], true);
        if (FAILED(hr)) {
            prepared.clear();
            return early_failure(hr);
        }
        entry.first_cpu = static_cast<uint8_t*>(p.first_cpu_alias);
        entry.second_cpu = static_cast<uint8_t*>(p.second_cpu_alias);
        entry.source = p.source;
        entry.alias = p.alias;
        entry.heap_tile = chosen[i];
        prepared.push_back(std::move(entry));
    }

    // Publish complete ownership metadata BEFORE the first void-returning
    // native UpdateTileMappings, so any fence error leaves CPU pins, GPU heap
    // slots and stagers discoverable for unbind()/clear() retry.
    try {
        bindings_.reserve(bindings_.size() + count);
        for (size_t i = 0; i < count; ++i) {
            const auto& p = pages[i];
            gpu_slots_.emplace(p.source, p.frame);
            gpu_slots_.emplace(p.alias, p.frame);
            const auto [it, inserted] = bindings_.emplace(
                p.frame, std::move(prepared[i]));
            if (!inserted) throw std::bad_alloc();
            occupied_heap_tiles_[chosen[i]] = true;
        }
    } catch (...) {
        // No GPU map command has been issued: safe to discard newly created
        // prepared stagers and reverse exactly the newly acquired CPU pins.
        for (size_t i = 0; i < count; ++i) {
            const auto& p = pages[i];
            bindings_.erase(p.frame);
            gpu_slots_.erase(p.source);
            gpu_slots_.erase(p.alias);
            occupied_heap_tiles_[chosen[i]] = false;
        }
        prepared.clear();
        return early_failure(E_OUTOFMEMORY);
    }

    // If a native map rejects a request or a later GPU fence fails, retire
    // only newly registered pages whose stager close acknowledges completion.
    // Failed pages remain registered and pinned, with slots unavailable for
    // reuse; the caller can retry unbind/clear, never a false successful bind.
    const auto retire_new = [this, &pages, &chosen](HRESULT error) noexcept {
        for (size_t i = 0; i < pages.size(); ++i) {
            const auto& p = pages[i];
            auto it = bindings_.find(p.frame);
            if (it == bindings_.end()) continue;
            if (FAILED(it->second.stager->close())) continue;
            const bool secondary = physical_.unpin_mapped_alias(
                p.second_cpu_alias, p.frame);
            const bool primary = physical_.unpin_mapped_alias(
                p.first_cpu_alias, p.frame);
            if (!secondary || !primary) std::terminate();
            gpu_slots_.erase(p.source);
            gpu_slots_.erase(p.alias);
            occupied_heap_tiles_[chosen[i]] = false;
            bindings_.erase(it);
        }
        return error;
    };

    // Exactly TWO native UpdateTileMappings calls for N pages: the first
    // contiguous source region and the second alias in another resource.
    HRESULT hr = windows_.map_tile_selection(
        queue_.Get(), src.window, src.offset, shared_tile_heap_.Get(), chosen);
    if (FAILED(hr)) return retire_new(hr);
    for (const auto& p : pages)
        bindings_.at(p.frame).stager->first_mapped_ = true;
    hr = windows_.map_tile_selection(
        queue_.Get(), dst.window, dst.offset, shared_tile_heap_.Get(), chosen);
    if (FAILED(hr)) return retire_new(hr);
    for (const auto& p : pages)
        bindings_.at(p.frame).stager->second_mapped_ = true;

    if (test_fail_next_mapping_wait_) {
        test_fail_next_mapping_wait_ = false;
        // Synthetic timeout after two REAL native mapping enqueues; keep
        // entries pinned and GPU memory owned, pending a subsequent fence.
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    const uint64_t target = next_batch_fence_++;
    hr = queue_->Signal(batch_fence_.Get(), target);
    if (FAILED(hr)) return hr;
    const auto observe = [this, target]() noexcept {
        return classify_fence_observation(
            batch_fence_->GetCompletedValue(), target);
    };
    const auto device_lost = [this]() noexcept {
        const HRESULT reason = device_->GetDeviceRemovedReason();
        return FAILED(reason) ? reason : DXGI_ERROR_DEVICE_REMOVED;
    };
    auto status = observe();
    if (status == FenceObservation::device_removed) return device_lost();
    if (status == FenceObservation::pending) {
        hr = batch_fence_->SetEventOnCompletion(target, batch_event_);
        if (FAILED(hr)) return hr;
        const DWORD waited = WaitForSingleObject(batch_event_, 10000);
        if (waited != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(
                waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    }
    status = observe();
    if (status == FenceObservation::device_removed) return device_lost();
    if (status != FenceObservation::completed) return E_FAIL;

    // Only after real GPU acknowledgement can page transfers become active.
    for (const auto& p : pages)
        bindings_.at(p.frame).stager->opened_ = true;
    return S_OK;
}

void GuestGpuPageRegistry::inject_next_mapping_wait_failure_for_test() noexcept {
    std::lock_guard lock(mutex_);
    test_fail_next_mapping_wait_ = true;
}

uint64_t GuestGpuPageRegistry::native_mapping_count() const noexcept {
    std::lock_guard lock(mutex_);
    return windows_.native_map_update_calls();
}
uint64_t GuestGpuPageRegistry::native_unmapping_count() const noexcept {
    std::lock_guard lock(mutex_);
    return windows_.native_unmap_update_calls();
}

void GuestGpuPageRegistry::inject_next_unmap_failure_after_source_for_test() noexcept {
    std::lock_guard lock(mutex_);
    test_fail_next_unmap_after_source_ = true;
}

void GuestGpuPageRegistry::inject_next_unmap_failure_after_both_for_test() noexcept {
    std::lock_guard lock(mutex_);
    test_fail_next_unmap_after_both_ = true;
}

// Fenced native NULL tile-range unmap for a complete, contiguous cohort.
// Each source/alias group belongs to one D3D12 reserved resource. This
// intentionally leaves per-frame unbind and clear() available for arbitrary
// noncontiguous bindings and as recovery after a partial update failure.
HRESULT GuestGpuPageRegistry::unbind_batch(
    std::span<const uintptr_t> frames) noexcept {
    constexpr uint64_t tile = GpuPageStager::page_bytes;
    const size_t count = frames.size();
    if (!count || count > heap_capacity_tiles) return E_INVALIDARG;
    if (!device_ || !queue_) return E_UNEXPECTED;

    std::lock_guard lock(mutex_);
    std::array<Binding*, heap_capacity_tiles> selected{};
    std::array<GpuPageStager*, heap_capacity_tiles> stagers{};
    const auto first = bindings_.find(frames.front());
    if (first == bindings_.end())
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    const GpuSlot source = first->second.source;
    const GpuSlot alias = first->second.alias;
    if (source.offset > UINT64_MAX - (count - 1) * tile ||
        alias.offset > UINT64_MAX - (count - 1) * tile)
        return E_INVALIDARG;
    uint64_t ignored_source = 0, ignored_alias = 0;
    ID3D12Resource* src_resource = windows_.tile_resource(
        source.window, source.offset, &ignored_source);
    ID3D12Resource* alias_resource = windows_.tile_resource(
        alias.window, alias.offset, &ignored_alias);
    if (!src_resource || !alias_resource || src_resource == alias_resource)
        return E_INVALIDARG;

    bool source_mapped = first->second.stager->first_mapped_;
    bool alias_mapped = first->second.stager->second_mapped_;
    for (size_t i = 0; i < count; ++i) {
        const auto found = bindings_.find(frames[i]);
        if (found == bindings_.end())
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        auto& entry = found->second;
        if (entry.source.window != source.window ||
            entry.alias.window != alias.window ||
            entry.source.offset != source.offset + i * tile ||
            entry.alias.offset != alias.offset + i * tile ||
            entry.stager->first_mapped_ != source_mapped ||
            entry.stager->second_mapped_ != alias_mapped)
            return E_INVALIDARG;
        uint64_t src_offset = 0, dst_offset = 0;
        if (windows_.tile_resource(source.window, entry.source.offset,
                                   &src_offset) != src_resource ||
            windows_.tile_resource(alias.window, entry.alias.offset,
                                   &dst_offset) != alias_resource)
            return E_INVALIDARG;
        for (size_t j = 0; j < i; ++j)
            if (frames[j] == frames[i]) return E_INVALIDARG;
        selected[i] = &entry;
        stagers[i] = entry.stager.get();
    }

    // Establish that ALL earlier submitted per-frame work is complete
    // before modifying either native resource's tile residency. If any
    // wait fails no new NULL tile-mapping operation has been queued.
    for (size_t i = 0; i < count; ++i) {
        auto* s = stagers[i];
        if (s->queue_ && s->fence_ && s->event_) {
            const HRESULT hr = s->sync();
            if (FAILED(hr)) return hr;
            s->in_flight_ = false;
        } else if (s->windows_ || source_mapped || alias_mapped) {
            return E_UNEXPECTED;
        }
    }

    // From here the cohort is unavailable to normal GPU transfers, even
    // after a failure halfway through NULL mapping. Existing CPU pins and
    // occupied physical heap tiles remain until every NULL unmap is fenced.
    for (size_t i = 0; i < count; ++i) {
        stagers[i]->opened_ = false;
        stagers[i]->prepared_ = false;
    }

    if (source_mapped) {
        const HRESULT hr = windows_.unmap_tiles(
            queue_.Get(), source.window, source.offset,
            static_cast<uint32_t>(count));
        if (FAILED(hr)) return hr;
        for (size_t i = 0; i < count; ++i)
            stagers[i]->first_mapped_ = false;
        if (test_fail_next_unmap_after_source_) {
            test_fail_next_unmap_after_source_ = false;
            // Synthetic error AFTER a REAL native NULL tile enqueue;
            // retry must not unmap this group again.
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
    }
    if (alias_mapped) {
        const HRESULT hr = windows_.unmap_tiles(
            queue_.Get(), alias.window, alias.offset,
            static_cast<uint32_t>(count));
        if (FAILED(hr)) return hr;
        for (size_t i = 0; i < count; ++i)
            stagers[i]->second_mapped_ = false;
        if (test_fail_next_unmap_after_both_) {
            test_fail_next_unmap_after_both_ = false;
            // Queued TWO actual D3D12 NULL mapping operations. Preserve
            // the entire cohort until a later successful completion fence.
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
    }

    if (!batch_fence_) {
        const HRESULT hr = device_->CreateFence(
            0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(batch_fence_.GetAddressOf()));
        if (FAILED(hr)) return hr;
    }
    if (!batch_event_) {
        batch_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!batch_event_) return HRESULT_FROM_WIN32(GetLastError());
    }
    if (next_batch_fence_ == UINT64_MAX)
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    const uint64_t target = next_batch_fence_++;
    HRESULT hr = queue_->Signal(batch_fence_.Get(), target);
    if (FAILED(hr)) return hr;
    const auto observe = [this, target]() noexcept {
        return classify_fence_observation(
            batch_fence_->GetCompletedValue(), target);
    };
    const auto removed = [this]() noexcept {
        const HRESULT reason = device_->GetDeviceRemovedReason();
        return FAILED(reason) ? reason : DXGI_ERROR_DEVICE_REMOVED;
    };
    auto status = observe();
    if (status == FenceObservation::device_removed) return removed();
    if (status == FenceObservation::pending) {
        hr = batch_fence_->SetEventOnCompletion(target, batch_event_);
        if (FAILED(hr)) return hr;
        const DWORD waited = WaitForSingleObject(batch_event_, 10000);
        if (waited != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(
                waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    }
    status = observe();
    if (status == FenceObservation::device_removed) return removed();
    if (status != FenceObservation::completed) return E_FAIL;

    // The complete cohort's NULL mappings have now been acknowledged.
    // If a per-stager close fails, no CPU aliases are unpinned, so a retry
    // or the registry's existing clear() can safely finish retirement.
    for (size_t i = 0; i < count; ++i) {
        if (stagers[i]->windows_) {
            hr = stagers[i]->close();
            if (FAILED(hr)) return hr;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        const auto frame = frames[i];
        auto& entry = *selected[i];
        const bool mirror = physical_.unpin_mapped_alias(
            entry.second_cpu, frame);
        const bool primary = physical_.unpin_mapped_alias(
            entry.first_cpu, frame);
        // Unpinning a validated live mapping cannot fail in normal use;
        // fail-stop rather than silently recycle an incompletely pinned VA.
        if (!mirror || !primary) std::terminate();
        gpu_slots_.erase(entry.source);
        gpu_slots_.erase(entry.alias);
        occupied_heap_tiles_[entry.heap_tile] = false;
        bindings_.erase(frame);
    }
    return S_OK;
}

} // namespace scarlett::gpu
