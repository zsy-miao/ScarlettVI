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

    HRESULT allocate(void** output, void* heap, uint32_t bytes,
                     uint32_t alignment) noexcept {
        if (!output)
            return E_POINTER;
        *output = nullptr;

        // No xbox APU heap has been implemented. A non-null heap must not
        // be mistaken for the default host allocation domain.
        if (heap)
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
            if (live_bytes_ > kHostApuMaxResidentBytes - bytes)
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
        return {live_bytes_, allocations_.size()};
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
};
} // namespace

extern "C" HRESULT WINAPI ApuAlloc(void** output, void* heap,
                                    uint32_t bytes,
                                    uint32_t alignment) noexcept {
    return HostApuAllocator::instance().allocate(
        output, heap, bytes, alignment);
}

extern "C" void WINAPI ApuFree(void* memory) noexcept {
    HostApuAllocator::instance().release(memory);
}

FARPROC find_acphal_export(std::string_view name) noexcept {
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
