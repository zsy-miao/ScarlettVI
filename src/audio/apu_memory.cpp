#include "apu_memory.h"
#include <malloc.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace scarlett::audio {
namespace {

class HostApuAllocator final {
public:
    static HostApuAllocator& instance() noexcept {
        static HostApuAllocator state;
        return state;
    }

    HRESULT configure_heap(uint32_t cached, uint32_t noncached) noexcept {
        if (cached == 0 && noncached == 0)
            return E_INVALIDARG;
        try {
            const std::lock_guard lock(mutex_);
            if (heap_configured_ && cached == cached_budget_ &&
                noncached == noncached_budget_)
                return S_OK;
            // Never change a live allocation's accounting domain.
            if (live_bytes_ != 0)
                return HRESULT_FROM_WIN32(ERROR_BUSY);
            cached_budget_ = cached;
            noncached_budget_ = noncached;
            heap_configured_ = true;
            // Reservations are *accounting* only: no Xbox hardware memory
            // is committed. Preserve an independent hard host cap.
            const uint64_t sum = uint64_t{cached} + noncached;
            allocation_ceiling_ =
                std::min<uint64_t>(sum, kHostApuMaxResidentBytes);
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    HRESULT allocate(void** output, void* device_address_output,
                     uint32_t bytes, uint32_t alignment,
                     uint32_t flags) noexcept {
        if (!output)
            return E_POINTER;
        *output = nullptr;

        // A requested 32-bit APU device-address output and unknown flags
        // have no verified host equivalent. Never hand out a fake address.
        if (device_address_output || flags != 0)
            return E_NOTIMPL;
        if (bytes == 0 || bytes > kHostApuMaxSingleAllocation ||
            alignment < 4u || alignment > 65536u ||
            (alignment & (alignment - 1u)) != 0)
            return E_INVALIDARG;

        // A Windows x64 host pointer is naturally aligned to at least 8.
        const size_t host_alignment =
            std::max<size_t>(alignment, alignof(void*));

        try {
            const std::lock_guard lock(mutex_);
            if (bytes > allocation_ceiling_ ||
                live_bytes_ > allocation_ceiling_ - bytes)
                return E_OUTOFMEMORY;

            void* memory = _aligned_malloc(bytes, host_alignment);
            if (!memory)
                return E_OUTOFMEMORY;
            // Zeroing is a safe deterministic desktop policy, not an
            // assertion about initial console APU memory contents.
            std::memset(memory, 0, bytes);

            try {
                const auto [it, inserted] = allocations_.emplace(
                    memory, bytes);
                (void)it;
                if (!inserted) {
                    _aligned_free(memory);
                    return E_FAIL;
                }
            } catch (...) {
                _aligned_free(memory);
                return E_OUTOFMEMORY;
            }
            live_bytes_ += bytes;
            *output = memory;
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    void release(void* memory) noexcept {
        if (!memory)
            return;
        // Never release memory outside our registry (including double
        // frees, foreign title allocations, and interior pointers).
        try {
            const std::lock_guard lock(mutex_);
            const auto found = allocations_.find(memory);
            if (found == allocations_.end())
                return;
            live_bytes_ -= found->second;
            allocations_.erase(found);
            _aligned_free(memory);
        } catch (...) {
            // ApuFree's guest-visible ABI has no recoverable output.
        }
    }

    HostApuAllocationStats stats() noexcept {
        const std::lock_guard lock(mutex_);
        return {live_bytes_, allocations_.size(), allocation_ceiling_,
                cached_budget_, noncached_budget_, heap_configured_};
    }

    ~HostApuAllocator() {
        for (const auto& [memory, bytes] : allocations_) {
            (void)bytes;
            _aligned_free(memory);
        }
    }
private:
    std::mutex mutex_;
    std::unordered_map<void*, uint32_t> allocations_;
    uint64_t live_bytes_ = 0;
    uint64_t allocation_ceiling_ = kHostApuMaxResidentBytes;
    uint32_t cached_budget_ = 0;
    uint32_t noncached_budget_ = 0;
    bool heap_configured_ = false;
};
} // namespace

extern "C" HRESULT WINAPI ApuCreateHeap(
    uint32_t cached_bytes, uint32_t noncached_bytes) noexcept {
    return HostApuAllocator::instance().configure_heap(
        cached_bytes, noncached_bytes);
}

extern "C" HRESULT WINAPI ApuAlloc(void** output,
                                    void* device_address_output,
                                    uint32_t bytes, uint32_t alignment,
                                    uint32_t flags) noexcept {
    return HostApuAllocator::instance().allocate(
        output, device_address_output, bytes, alignment, flags);
}

extern "C" void WINAPI ApuFree(void* memory) noexcept {
    HostApuAllocator::instance().release(memory);
}

FARPROC find_acphal_export(std::string_view name) noexcept {
    if (name == "ApuCreateHeap")
        return reinterpret_cast<FARPROC>(&ApuCreateHeap);
    if (name == "ApuAlloc")
        return reinterpret_cast<FARPROC>(&ApuAlloc);
    if (name == "ApuFree")
        return reinterpret_cast<FARPROC>(&ApuFree);
    return nullptr;
}

HostApuAllocationStats host_apu_allocation_stats() noexcept {
    return HostApuAllocator::instance().stats();
}
} // namespace scarlett::audio
