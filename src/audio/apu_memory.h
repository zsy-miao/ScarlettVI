#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace scarlett::audio {

// Real Windows CPU-backed APU allocation *subset*. The supplied game's
// verified ApuAlloc call passes an out pointer, null heap, byte count and
// 0x800 alignment. This host allocator is NOT an Xbox APU physical/virtual
// address mapper, DSP memory, or a replacement for ApuCreateHeap.
struct HostApuAllocationStats {
    uint64_t allocated_bytes = 0;
    size_t live_allocations = 0;
};

// Conservative per-process host limits, NOT Xbox hardware budgets.
inline constexpr uint32_t kHostApuMaxSingleAllocation = 64u * 1024u * 1024u;
inline constexpr uint64_t kHostApuMaxResidentBytes = 256ull * 1024ull * 1024ull;

extern "C" {
HRESULT WINAPI ApuAlloc(void** output, void* heap, uint32_t bytes,
                        uint32_t alignment) noexcept;
void WINAPI ApuFree(void* pointer) noexcept;
}

// Exact-name AcpHal.dll import router; unknown and non-null-heap functions
// are deliberately not faked. Ordinals are never guessed.
FARPROC find_acphal_export(std::string_view name) noexcept;
HostApuAllocationStats host_apu_allocation_stats() noexcept;
bool verify_host_apu_allocation_fixtures() noexcept;
} // namespace scarlett::audio
