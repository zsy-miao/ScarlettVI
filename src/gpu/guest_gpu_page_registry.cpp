#include "guest_gpu_page_registry.h"
#include <cstring>
#include <new>

namespace scarlett::gpu {
GuestGpuPageRegistry::GuestGpuPageRegistry(
    memory::PhysicalMemory& physical, SparseGpuWindows& windows,
    ID3D12Device11* device) noexcept
    : physical_(physical), windows_(windows), device_(device) {}

GuestGpuPageRegistry::~GuestGpuPageRegistry() { clear(); }

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
        gpu_slots_.emplace(source_gpu, frame);
        gpu_slots_.emplace(alias_gpu, frame);
        auto stager = std::make_unique<GpuPageStager>();
        result = stager->open(
            device_.Get(), windows_,
            source_gpu.window, source_gpu.offset,
            alias_gpu.window, alias_gpu.offset);
        if (SUCCEEDED(result)) {
            Binding entry{};
            entry.first_cpu = static_cast<uint8_t*>(first_cpu_alias);
            entry.second_cpu = static_cast<uint8_t*>(second_cpu_alias);
            entry.source = source_gpu;
            entry.alias = alias_gpu;
            entry.stager = std::move(stager);
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
        output.size() != GpuPageStager::page_bytes ||
        overlaps_cpu(it->second, output))
        return E_INVALIDARG;

    // The GpuPageStager validates input/output overlap and completes its
    // real D3D12 fence BEFORE modifying output. CPU XMem writeback is a
    // separate, opt-in operation and happens only on a successful transfer.
    const HRESULT result = it->second.stager->roundtrip(input, output);
    if (FAILED(result)) return result;
    if (writeback_cpu)
        std::memcpy(it->second.second_cpu, output.data(),
                    GpuPageStager::page_bytes);
    ++completed_transfers_;
    return S_OK;
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
    bindings_.erase(it);
    return S_OK;
}

HRESULT GuestGpuPageRegistry::clear() noexcept {
    std::lock_guard lock(mutex_);
    HRESULT first_error = S_OK;
    for (auto& [frame, binding] : bindings_) {
        const HRESULT result = binding.stager->close();
        if (FAILED(result) && SUCCEEDED(first_error)) first_error = result;
        if (!physical_.unpin_mapped_alias(binding.second_cpu, frame) &&
            SUCCEEDED(first_error)) first_error = E_FAIL;
        if (!physical_.unpin_mapped_alias(binding.first_cpu, frame) &&
            SUCCEEDED(first_error)) first_error = E_FAIL;
    }
    bindings_.clear();
    gpu_slots_.clear();
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
