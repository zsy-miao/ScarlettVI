#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace scarlett::audio {

// Real Windows CPU-backed APU allocation *subset*. The supplied game's
// verified ApuAlloc call passes a virtual-pointer output, null optional
// device-address output, byte count, 0x800 alignment and a fifth DWORD.
// The address bridge below exposes 32-bit guest addresses for *owned*
// host allocations only. It is NOT an Xbox physical/DSP-memory mapper.
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
// 0 is the only supported host policy. Optional RDX output is a 32-bit
// address in the *owned* host APU mapping, not physical console DMA RAM.
HRESULT WINAPI ApuAlloc(void** output, uint32_t* device_address_output,
                        uint32_t bytes, uint32_t alignment,
                        uint32_t flags) noexcept;
void WINAPI ApuFree(void* pointer) noexcept;
// The actual title callsites pass (RCX=pointer,RDX=span), test AL,
// store EAX for CPU -> APU and test RAX for APU -> CPU respectively.
// Only ranges owned by ApuAlloc are mapped; no arbitrary Win32 memory.
BOOL WINAPI ApuIsVirtualAddressValid(const void* host_address,
                                     uint32_t span_bytes) noexcept;
uint32_t WINAPI ApuMapVirtualAddress(const void* host_address) noexcept;
void* WINAPI ApuMapApuAddress(uint32_t apu_address) noexcept;
}

// Exact-name AcpHal.dll import router; unowned virtual-memory and APU
// address requests remain unsupported. Ordinals are never guessed.
FARPROC find_acphal_export(std::string_view name) noexcept;
HostApuAllocationStats host_apu_allocation_stats() noexcept;
bool verify_host_apu_allocation_fixtures() noexcept;
bool verify_host_apu_address_fixtures() noexcept;
} // namespace scarlett::audio
