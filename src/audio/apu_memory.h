#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace scarlett::audio {

// Real Windows CPU-backed APU allocation *subset*. The supplied game's
// verified ApuAlloc call passes a virtual-pointer output, null optional
// device-address output, byte count, 0x800 alignment and a fifth DWORD.
// Host heap budgeting is not Xbox APU physical/virtual mapping or DSP RAM.
struct HostApuAllocationStats {
    uint64_t allocated_bytes = 0;
    size_t live_allocations = 0;
    uint64_t allocation_ceiling_bytes = 0;
    uint32_t cached_budget_bytes = 0;
    uint32_t noncached_budget_bytes = 0;
    bool heap_configured = false;
};

// Conservative per-process host limits, NOT Xbox hardware budgets.
inline constexpr uint32_t kHostApuMaxSingleAllocation = 64u * 1024u * 1024u;
inline constexpr uint64_t kHostApuMaxResidentBytes = 256ull * 1024ull * 1024ull;

extern "C" {
// The title's 0x1443AD933 import call supplies two 32-bit sizes and
// checks EAX as HRESULT. Independent public research identifies them
// as cached/noncached budget sizes. Host-side quota only: no GPU/APU RAM.
HRESULT WINAPI ApuCreateHeap(uint32_t cached_bytes,
                             uint32_t noncached_bytes) noexcept;
// Current title passes a fifth DWORD in the outgoing stack argument slot;
// 0 is the only validated host policy. Other flag variants remain unsupported.
HRESULT WINAPI ApuAlloc(void** output, void* device_address_output,
                        uint32_t bytes, uint32_t alignment,
                        uint32_t flags) noexcept;
void WINAPI ApuFree(void* pointer) noexcept;
}

// Exact-name AcpHal.dll import router; unknown and non-null device-address
// output requests are deliberately not faked. Ordinals are never guessed.
FARPROC find_acphal_export(std::string_view name) noexcept;
HostApuAllocationStats host_apu_allocation_stats() noexcept;
bool verify_host_apu_allocation_fixtures() noexcept;
} // namespace scarlett::audio
