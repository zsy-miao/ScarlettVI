#include "apu_memory.h"
#include "apu_shape.h"
#include "acp_hal_host.h"
#include <malloc.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <map>

namespace scarlett::audio {
namespace {
struct OwnedApuMemory {
    uint32_t bytes = 0;
    uint32_t apu_start = 0;
};
constexpr uint64_t kApuAddressLimit = uint64_t{1} << 32;
constexpr uint64_t kApuFirstAddress = 0x10000;

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

    HRESULT allocate(void** output, uint32_t* device_address_output,
                     uint32_t bytes, uint32_t alignment,
                     uint32_t flags) noexcept {
        if (!output)
            return E_POINTER;
        *output = nullptr;
        if (device_address_output)
            *device_address_output = 0;
        // Only flags=0 is verified. An optional APU-address output
        // references the same real, owned mapping as ApuMapVirtualAddress.
        if (flags != 0)
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

            // Give each owned CPU allocation a unique, nonzero 32-bit
            // APU VA. Do NOT recycle released ranges; stale device
            // addresses cannot accidentally resolve a newer allocation.
            // This is host-only address translation, not physical APU RAM.
            const uint64_t apu_alignment =
                std::max<uint64_t>(alignment, 4096);
            const uint64_t apu_start =
                (next_apu_address_ + apu_alignment - 1) &
                ~(apu_alignment - 1);
            const uint64_t apu_end = apu_start + bytes;
            if (apu_start >= kApuAddressLimit ||
                apu_end > kApuAddressLimit)
                return E_OUTOFMEMORY;
            void* memory = _aligned_malloc(bytes, host_alignment);
            if (!memory)
                return E_OUTOFMEMORY;
            // Zeroing is a safe deterministic desktop policy, not an
            // assertion about initial console APU memory contents.
            std::memset(memory, 0, bytes);

            try {
                const auto [it, inserted] = allocations_.emplace(
                    reinterpret_cast<uintptr_t>(memory),
                    OwnedApuMemory{bytes, static_cast<uint32_t>(apu_start)});
                if (!inserted) {
                    _aligned_free(memory);
                    return E_FAIL;
                }
                try {
                    const auto [map_it, mapped] = apu_to_host_.emplace(
                        static_cast<uint32_t>(apu_start),
                        reinterpret_cast<uintptr_t>(memory));
                    (void)map_it;
                    if (!mapped) {
                        allocations_.erase(it);
                        _aligned_free(memory);
                        return E_FAIL;
                    }
                } catch (...) {
                    allocations_.erase(it);
                    throw;
                }
            } catch (...) {
                _aligned_free(memory);
                return E_OUTOFMEMORY;
            }
            next_apu_address_ = (apu_end + 4095) & ~uint64_t{4095};
            live_bytes_ += bytes;
            *output = memory;
            if (device_address_output)
                *device_address_output = static_cast<uint32_t>(apu_start);
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
            const auto found =
                allocations_.find(reinterpret_cast<uintptr_t>(memory));
            if (found == allocations_.end())
                return;
            live_bytes_ -= found->second.bytes;
            apu_to_host_.erase(found->second.apu_start);
            allocations_.erase(found);
            _aligned_free(memory);
        } catch (...) {
            // ApuFree's guest-visible ABI has no recoverable output.
        }
    }

    BOOL valid_virtual(const void* address, uint32_t bytes) noexcept {
        if (!address || bytes == 0)
            return FALSE;
        try {
            const std::lock_guard lock(mutex_);
            const auto found = owner_for_host(
                reinterpret_cast<uintptr_t>(address));
            if (found == allocations_.end())
                return FALSE;
            const uintptr_t offset =
                reinterpret_cast<uintptr_t>(address) - found->first;
            return bytes <= found->second.bytes - offset ? TRUE : FALSE;
        } catch (...) {
            return FALSE;
        }
    }

    uint32_t map_virtual(const void* address) noexcept {
        if (!address)
            return 0;
        try {
            const std::lock_guard lock(mutex_);
            const auto found = owner_for_host(
                reinterpret_cast<uintptr_t>(address));
            if (found == allocations_.end())
                return 0;
            const uint32_t offset = static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(address) - found->first);
            return found->second.apu_start + offset;
        } catch (...) {
            return 0;
        }
    }

    void* map_apu(uint32_t address) noexcept {
        if (!address)
            return nullptr;
        try {
            const std::lock_guard lock(mutex_);
            auto found = apu_to_host_.upper_bound(address);
            if (found == apu_to_host_.begin())
                return nullptr;
            --found;
            const uint32_t offset = address - found->first;
            const auto alloc = allocations_.find(found->second);
            if (alloc == allocations_.end() ||
                offset >= alloc->second.bytes)
                return nullptr;
            return reinterpret_cast<void*>(found->second + offset);
        } catch (...) {
            return nullptr;
        }
    }

    HostApuAllocationStats stats() noexcept {
        const std::lock_guard lock(mutex_);
        return {live_bytes_, allocations_.size(), allocation_ceiling_,
                cached_budget_, noncached_budget_, heap_configured_};
    }

    ~HostApuAllocator() {
        for (const auto& [memory, alloc] : allocations_) {
            (void)alloc;
            _aligned_free(reinterpret_cast<void*>(memory));
        }
    }
private:
    using HostMap = std::map<uintptr_t, OwnedApuMemory>;
    HostMap::const_iterator owner_for_host(uintptr_t address) const noexcept {
        auto it = allocations_.upper_bound(address);
        if (it == allocations_.begin())
            return allocations_.end();
        --it;
        return address - it->first < it->second.bytes
            ? it : allocations_.end();
    }

    std::mutex mutex_;
    HostMap allocations_;
    std::map<uint32_t, uintptr_t> apu_to_host_;
    uint64_t next_apu_address_ = kApuFirstAddress;
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
                                    uint32_t* device_address_output,
                                    uint32_t bytes, uint32_t alignment,
                                    uint32_t flags) noexcept {
    return HostApuAllocator::instance().allocate(
        output, device_address_output, bytes, alignment, flags);
}

extern "C" void WINAPI ApuFree(void* memory) noexcept {
    HostApuAllocator::instance().release(memory);
}
extern "C" BOOL WINAPI ApuIsVirtualAddressValid(
    const void* address, uint32_t bytes) noexcept {
    return HostApuAllocator::instance().valid_virtual(address, bytes);
}
extern "C" uint32_t WINAPI ApuMapVirtualAddress(
    const void* address) noexcept {
    return HostApuAllocator::instance().map_virtual(address);
}
extern "C" void* WINAPI ApuMapApuAddress(uint32_t address) noexcept {
    return HostApuAllocator::instance().map_apu(address);
}

FARPROC find_acphal_export(std::string_view name) noexcept {
    if (name == "ApuCreateHeap")
        return reinterpret_cast<FARPROC>(&ApuCreateHeap);
    if (name == "ApuAlloc")
        return reinterpret_cast<FARPROC>(&ApuAlloc);
    if (name == "ApuFree")
        return reinterpret_cast<FARPROC>(&ApuFree);
    if (name == "ApuIsVirtualAddressValid")
        return reinterpret_cast<FARPROC>(&ApuIsVirtualAddressValid);
    if (name == "ApuMapVirtualAddress")
        return reinterpret_cast<FARPROC>(&ApuMapVirtualAddress);
    if (name == "ApuMapApuAddress")
        return reinterpret_cast<FARPROC>(&ApuMapApuAddress);
    if (FARPROC address = find_shape_export(name))
        return address;
    return find_acphal_factory_export(name);
}

HostApuAllocationStats host_apu_allocation_stats() noexcept {
    return HostApuAllocator::instance().stats();
}
} // namespace scarlett::audio
