#include "guest_gpu_page_registry.h"
#include "fence_safety.h"
#include <cstring>
#include <algorithm>
#include <array>
#include <new>
#include <exception>
#include <unordered_set>

namespace scarlett::gpu {
GuestGpuPageRegistry::GuestGpuPageRegistry(
    memory::PhysicalMemory& physical, SparseGpuWindows& windows,
    ID3D12Device11* device) noexcept
    : physical_(physical), windows_(windows), device_(device) {}

GuestGpuPageRegistry::~GuestGpuPageRegistry() {
    // The caller must resolve any failed clear() before destroying the
    // backing XMem manager and sparse D3D12 VA windows. Fail-stop otherwise.
    if (FAILED(clear())) std::terminate();
}

bool GuestGpuPageRegistry::overlaps_cpu(
    const Binding& binding, std::span<uint8_t> output) const noexcept {
    const uintptr_t dst = reinterpret_cast<uintptr_t>(output.data());
    for (const uint8_t* pointer : {binding.first_cpu, binding.second_cpu}) {
        const uintptr_t src = reinterpret_cast<uintptr_t>(pointer);
        if (src <= dst ? dst - src < GpuPageStager::page_bytes
                       : src - dst < GpuPageStager::page_bytes)
            return true;
    }
    return false;
}

HRESULT GuestGpuPageRegistry::bind(
    uintptr_t frame, void* first_cpu_alias, void* second_cpu_alias,
    GpuSlot source_gpu, GpuSlot alias_gpu) noexcept {
    constexpr size_t page = GpuPageStager::page_bytes;
    if (!device_) return E_POINTER;
    if (!frame || !first_cpu_alias || !second_cpu_alias ||
        first_cpu_alias == second_cpu_alias ||
        (reinterpret_cast<uintptr_t>(first_cpu_alias) & (page - 1)) ||
        (reinterpret_cast<uintptr_t>(second_cpu_alias) & (page - 1)) ||
        source_gpu == alias_gpu ||
        (source_gpu.offset & (page - 1)) ||
        (alias_gpu.offset & (page - 1)))
        return E_INVALIDARG;

    std::lock_guard lock(mutex_);
    if (bindings_.contains(frame))
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    if (gpu_slots_.contains(source_gpu) || gpu_slots_.contains(alias_gpu))
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    const auto candidate = std::find(
        occupied_heap_tiles_.begin(), occupied_heap_tiles_.end(), false);
    if (candidate == occupied_heap_tiles_.end())
        return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
    const uint32_t heap_tile = static_cast<uint32_t>(
        candidate - occupied_heap_tiles_.begin());

    if (!physical_.pin_mapped_alias(first_cpu_alias, frame))
        return HRESULT_FROM_WIN32(
            GetLastError() ? GetLastError() : ERROR_INVALID_ADDRESS);
    if (!physical_.pin_mapped_alias(second_cpu_alias, frame)) {
        const DWORD error = GetLastError() ? GetLastError() : ERROR_INVALID_ADDRESS;
        physical_.unpin_mapped_alias(first_cpu_alias, frame);
        return HRESULT_FROM_WIN32(error);
    }

    HRESULT result = S_OK;
    // Pins are kept throughout native GPU allocation and tile mapping.
    // If any step fails, every created GPU object and CPU pin is rolled back.
    try {
        if (!queue_) {
            D3D12_COMMAND_QUEUE_DESC desc{};
            desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            result = device_->CreateCommandQueue(
                &desc, IID_PPV_ARGS(queue_.GetAddressOf()));
            if (FAILED(result)) {
                physical_.unpin_mapped_alias(second_cpu_alias, frame);
                physical_.unpin_mapped_alias(first_cpu_alias, frame);
                return result;
            }
        }
        if (!shared_tile_heap_) {
            D3D12_HEAP_DESC heap{};
            heap.SizeInBytes =
                uint64_t{heap_capacity_tiles} * GpuPageStager::page_bytes;
            heap.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            heap.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            heap.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            result = device_->CreateHeap(
                &heap, IID_PPV_ARGS(shared_tile_heap_.GetAddressOf()));
            if (FAILED(result)) {
                physical_.unpin_mapped_alias(second_cpu_alias, frame);
                physical_.unpin_mapped_alias(first_cpu_alias, frame);
                return result;
            }
        }
        gpu_slots_.emplace(source_gpu, frame);
        gpu_slots_.emplace(alias_gpu, frame);
        auto stager = std::make_unique<GpuPageStager>();
        result = stager->open(
            device_.Get(), windows_,
            source_gpu.window, source_gpu.offset,
            alias_gpu.window, alias_gpu.offset, queue_.Get(),
            shared_tile_heap_.Get(), heap_tile);
        if (SUCCEEDED(result)) {
            Binding entry{};
            entry.first_cpu = static_cast<uint8_t*>(first_cpu_alias);
            entry.second_cpu = static_cast<uint8_t*>(second_cpu_alias);
            entry.source = source_gpu;
            entry.alias = alias_gpu;
            entry.heap_tile = heap_tile;
            entry.stager = std::move(stager);
            occupied_heap_tiles_[heap_tile] = true;
            const auto [it, inserted] = bindings_.emplace(
                frame, std::move(entry));
            if (inserted) return S_OK;
            result = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        }
    } catch (...) {
        result = E_OUTOFMEMORY;
    }
    gpu_slots_.erase(source_gpu);
    gpu_slots_.erase(alias_gpu);
    occupied_heap_tiles_[heap_tile] = false;
    physical_.unpin_mapped_alias(second_cpu_alias, frame);
    physical_.unpin_mapped_alias(first_cpu_alias, frame);
    return result;
}

HRESULT GuestGpuPageRegistry::transfer(
    uintptr_t frame, std::span<const uint8_t> input,
    std::span<uint8_t> output, bool writeback_cpu) noexcept {
    std::lock_guard lock(mutex_);
    const auto it = bindings_.find(frame);
    if (it == bindings_.end()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (input.size() != GpuPageStager::page_bytes ||
        output.size() != GpuPageStager::page_bytes)
        return E_INVALIDARG;
    // A caller must never replace ANOTHER registered page's pinned CPU
    // alias with scratch GPU readback from this frame.
    for (const auto& [registered_frame, entry] : bindings_)
        if (overlaps_cpu(entry, output))
            return E_INVALIDARG;

    // The GpuPageStager validates input/output overlap and completes its
    // real D3D12 fence BEFORE modifying output. CPU XMem writeback is a
    // separate, opt-in operation and happens only on a successful transfer.
    const uint64_t previous_submissions = it->second.stager->submissions();
    const HRESULT result = it->second.stager->roundtrip(input, output);
    // Count actual queue submission even when a later fence wait fails.
    if (it->second.stager->submissions() != previous_submissions)
        ++gpu_submissions_;
    if (FAILED(result)) return result;
    if (writeback_cpu)
        std::memcpy(it->second.second_cpu, output.data(),
                    GpuPageStager::page_bytes);
    ++completed_transfers_;
    return S_OK;
}

// A real host batch: N separate page command lists submitted together to the
// single registry-owned GPU queue, with one completion fence. Every request
// is checked before GPU work. The output and CPU writebacks are published
// only after ALL page GPU results are available.
HRESULT GuestGpuPageRegistry::transfer_batch(
    std::span<const PageTransfer> requests) noexcept {
    constexpr size_t page = GpuPageStager::page_bytes;
    if (requests.empty() || requests.size() > 64)
        return E_INVALIDARG;
    std::lock_guard lock(mutex_);
    if (!queue_) return E_UNEXPECTED;

    const auto intersects = [](const void* a, size_t a_size,
                               const void* b, size_t b_size) noexcept {
        const uintptr_t x = reinterpret_cast<uintptr_t>(a);
        const uintptr_t y = reinterpret_cast<uintptr_t>(b);
        return x <= y ? y - x < a_size : x - y < b_size;
    };

    // Full preflight: no missing/duplicate guest frames, no overlapping
    // output with ANY pinned CPU alias, input or another batch output.
    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        if (!bindings_.contains(request.frame))
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        if (request.input.size() != page || request.output.size() != page ||
            !request.input.data() || !request.output.data())
            return E_INVALIDARG;
        for (const auto& [frame, binding] : bindings_) {
            if (intersects(request.output.data(), page, binding.first_cpu, page) ||
                intersects(request.output.data(), page, binding.second_cpu, page))
                return E_INVALIDARG;
        }
        for (size_t j = 0; j < requests.size(); ++j) {
            const auto& other = requests[j];
            if ((j != i && request.frame == other.frame) ||
                (other.input.size() == page &&
                 intersects(request.output.data(), page,
                            other.input.data(), page)) ||
                (j != i && other.output.size() == page &&
                 intersects(request.output.data(), page,
                            other.output.data(), page)))
                return E_INVALIDARG;
        }
    }

    if (!batch_fence_) {
        HRESULT hr = device_->CreateFence(
            0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(batch_fence_.GetAddressOf()));
        if (FAILED(hr)) return hr;
    }
    if (!batch_event_) {
        batch_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!batch_event_)
            return HRESULT_FROM_WIN32(GetLastError());
    }
    if (next_batch_fence_ == UINT64_MAX)
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);

    std::vector<GpuPageStager*> stagers;
    std::vector<ID3D12CommandList*> lists;
    std::vector<std::array<uint8_t, page>> staged_results;
    try {
        stagers.reserve(requests.size());
        lists.reserve(requests.size());
        staged_results.resize(requests.size());
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    for (const auto& request : requests) {
        auto* stager = bindings_.at(request.frame).stager.get();
        const HRESULT hr = stager->prepare_roundtrip(request.input, request.output);
        if (FAILED(hr)) {
            for (auto* prepared : stagers) prepared->cancel_prepared();
            return hr;
        }
        stagers.push_back(stager);
        lists.push_back(stager->commands_.Get());
    }

    queue_->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
    ++gpu_submissions_; // actual submission, even if the wait later fails
    for (auto* stager : stagers) stager->mark_submitted();
    if (test_fail_next_batch_wait_) {
        test_fail_next_batch_wait_ = false;
        // Fault injection AFTER real GPU submission but BEFORE batch Signal.
        // No output is published; per-page close must fence on this queue.
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    const uint64_t fence_value = next_batch_fence_++;
    HRESULT hr = queue_->Signal(batch_fence_.Get(), fence_value);
    if (FAILED(hr)) return hr; // no readbacks or CPU writeback on failure
    const auto observe = [this, fence_value]() noexcept {
        return classify_fence_observation(
            batch_fence_->GetCompletedValue(), fence_value);
    };
    const auto removed = [this]() noexcept {
        const HRESULT reason = device_->GetDeviceRemovedReason();
        return FAILED(reason) ? reason : DXGI_ERROR_DEVICE_REMOVED;
    };
    auto status = observe();
    if (status == FenceObservation::device_removed) return removed();
    if (status == FenceObservation::pending) {
        hr = batch_fence_->SetEventOnCompletion(fence_value, batch_event_);
        if (FAILED(hr)) return hr;
        const DWORD result = WaitForSingleObject(batch_event_, 10000);
        if (result != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(
                result == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    }
    status = observe();
    if (status == FenceObservation::device_removed) return removed();
    if (status != FenceObservation::completed) return E_FAIL;
    // Do not publish any output until ALL native readback maps succeed.
    for (size_t i = 0; i < requests.size(); ++i) {
        hr = stagers[i]->collect_readback(staged_results[i]);
        if (FAILED(hr)) return hr;
    }
    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        std::memcpy(request.output.data(), staged_results[i].data(), page);
        if (request.writeback_cpu)
            std::memcpy(bindings_.at(request.frame).second_cpu,
                        staged_results[i].data(), page);
    }
    completed_transfers_ += requests.size();
    return S_OK;
}

uint64_t GuestGpuPageRegistry::queue_submission_count() const noexcept {
    std::lock_guard lock(mutex_);
    return gpu_submissions_;
}
size_t GuestGpuPageRegistry::native_queue_count() const noexcept {
    std::lock_guard lock(mutex_);
    return queue_ ? 1u : 0u;
}

size_t GuestGpuPageRegistry::physical_heap_count() const noexcept {
    std::lock_guard lock(mutex_);
    return shared_tile_heap_ ? 1u : 0u;
}

size_t GuestGpuPageRegistry::used_heap_tile_count() const noexcept {
    std::lock_guard lock(mutex_);
    return static_cast<size_t>(
        std::count(occupied_heap_tiles_.begin(), occupied_heap_tiles_.end(), true));
}

std::optional<uint32_t> GuestGpuPageRegistry::heap_tile_for(
    uintptr_t frame) const noexcept {
    std::lock_guard lock(mutex_);
    const auto it = bindings_.find(frame);
    if (it == bindings_.end()) return std::nullopt;
    return it->second.heap_tile;
}

void GuestGpuPageRegistry::inject_next_batch_wait_failure_for_test() noexcept {
    std::lock_guard lock(mutex_);
    test_fail_next_batch_wait_ = true;
}

bool GuestGpuPageRegistry::inject_next_gpu_wait_failure_for_test(
    uintptr_t frame, uint32_t successful_waits_before_failure) noexcept {
    if (successful_waits_before_failure > 2) return false;
    std::lock_guard lock(mutex_);
    const auto it = bindings_.find(frame);
    if (it == bindings_.end()) return false;
    it->second.stager->test_sync_failure_countdown_ =
        static_cast<int>(successful_waits_before_failure);
    return true;
}

HRESULT GuestGpuPageRegistry::unbind(uintptr_t frame) noexcept {
    std::lock_guard lock(mutex_);
    const auto it = bindings_.find(frame);
    if (it == bindings_.end()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    const HRESULT result = it->second.stager->close();
    if (FAILED(result)) return result;

    const bool mirror = physical_.unpin_mapped_alias(
        it->second.second_cpu, frame);
    const bool primary = physical_.unpin_mapped_alias(
        it->second.first_cpu, frame);
    if (!mirror || !primary) return E_FAIL;
    gpu_slots_.erase(it->second.source);
    gpu_slots_.erase(it->second.alias);
    occupied_heap_tiles_[it->second.heap_tile] = false;
    bindings_.erase(it);
    return S_OK;
}

HRESULT GuestGpuPageRegistry::clear() noexcept {
    std::lock_guard lock(mutex_);
    HRESULT first_error = S_OK;
    for (auto it = bindings_.begin(); it != bindings_.end();) {
        const uintptr_t frame = it->first;
        const HRESULT result = it->second.stager->close();
        if (FAILED(result)) {
            // Keep the entire binding and BOTH CPU pins. No GPU tile can be
            // reused while its final fence is still unconfirmed.
            if (SUCCEEDED(first_error)) first_error = result;
            ++it;
            continue;
        }
        // Both GPU tile aliases have been unmapped and their queue fence
        // completed. Releasing CPU pins is now safe.
        const bool mirror = physical_.unpin_mapped_alias(
            it->second.second_cpu, frame);
        const bool primary = physical_.unpin_mapped_alias(
            it->second.first_cpu, frame);
        if (!mirror || !primary) {
            // This indicates a programming invariant violation: the mapping
            // manager was altered out of band. Avoid freeing the live entry.
            if (SUCCEEDED(first_error)) first_error = E_FAIL;
            ++it;
            continue;
        }
        gpu_slots_.erase(it->second.source);
        gpu_slots_.erase(it->second.alias);
        occupied_heap_tiles_[it->second.heap_tile] = false;
        it = bindings_.erase(it);
    }
    if (!bindings_.empty())
        return FAILED(first_error) ? first_error : HRESULT_FROM_WIN32(ERROR_BUSY);
    // Only after every active page has retired can shared heaps, queues and
    // batch fences be released.
    gpu_slots_.clear();
    occupied_heap_tiles_.fill(false);
    shared_tile_heap_.Reset();
    batch_fence_.Reset();
    if (batch_event_) { CloseHandle(batch_event_); batch_event_ = nullptr; }
    queue_.Reset();
    next_batch_fence_ = 1;
    return first_error;
}

size_t GuestGpuPageRegistry::binding_count() const noexcept {
    std::lock_guard lock(mutex_);
    return bindings_.size();
}

uint64_t GuestGpuPageRegistry::submitted_transfers() const noexcept {
    std::lock_guard lock(mutex_);
    return completed_transfers_;
}

size_t GuestGpuPageRegistry::pinned_alias_count() const {
    return physical_.pinned_alias_count();
}
} // namespace scarlett::gpu
