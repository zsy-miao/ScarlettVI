#include "apu_shape.h"
#include "apu_memory.h"
#include <Windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace scarlett::audio {
namespace {
using AllocateFn = HRESULT (WINAPI*)(ShapeContextRequest*) noexcept;
using ReleaseFn = HRESULT (WINAPI*)() noexcept;
static_assert(std::is_same_v<decltype(&AcpHalAllocateShapeContexts), AllocateFn>);
static_assert(std::is_same_v<decltype(&AcpHalReleaseShapeContexts), ReleaseFn>);
}

bool verify_apu_shape_fixtures() noexcept {
    const auto alloc_export = find_shape_export("AcpHalAllocateShapeContexts");
    const auto release_export = find_shape_export("AcpHalReleaseShapeContexts");
    if (alloc_export != reinterpret_cast<FARPROC>(&AcpHalAllocateShapeContexts) ||
        release_export != reinterpret_cast<FARPROC>(&AcpHalReleaseShapeContexts) ||
        find_acphal_export("AcpHalAllocateShapeContexts") != alloc_export ||
        find_acphal_export("AcpHalReleaseShapeContexts") != release_export ||
        find_shape_export("AcpHalCreate") ||
        find_shape_export("AcpHalAllocateShapeContextsOrdinal") ||
        find_shape_export("acphalreleaseshapecontexts") ||
        find_shape_export(""))
        return false;
    const auto allocate = reinterpret_cast<AllocateFn>(alloc_export);
    const auto release = reinterpret_cast<ReleaseFn>(release_export);
    if (release() != S_OK || allocate(nullptr) != E_POINTER)
        return false;
    const auto before = host_apu_allocation_stats();
    if (before.live_allocations != 0)
        return false;

    ShapeContextRequest req{};
    req.counts[4] = 350;
    // Configure a real host heap that cannot accommodate the request.
    if (ApuCreateHeap(4096, 0) != S_OK ||
        allocate(&req) != E_OUTOFMEMORY ||
        req.arrays[4] != nullptr ||
        host_apu_allocation_stats().live_allocations != 0)
        return false;

    ShapeContextRequest invalid{};
    invalid.counts[0] = 1;
    invalid.counts[4] = 350;
    if (allocate(&invalid) != E_NOTIMPL ||
        invalid.arrays[0] || invalid.arrays[4])
        return false;
    invalid = {};
    invalid.counts[4] = kHostMaxShapeContexts + 1;
    if (allocate(&invalid) != E_INVALIDARG || invalid.arrays[4])
        return false;
    invalid = {};
    invalid.counts[4] = 350;
    invalid.arrays[0] = reinterpret_cast<void*>(uintptr_t{0x1234});
    if (allocate(&invalid) != E_INVALIDARG ||
        invalid.arrays[0] != reinterpret_cast<void*>(uintptr_t{0x1234}))
        return false;

    if (ApuCreateHeap(16384, 16384) != S_OK ||
        allocate(&req) != S_OK || !req.arrays[4])
        return false;
    const auto allocated = host_apu_allocation_stats();
    if (allocated.live_allocations != 1 ||
        allocated.allocated_bytes != 350u * 64u ||
        allocated.allocation_ceiling_bytes != 32768u ||
        !ApuIsVirtualAddressValid(req.arrays[4], 350u * 64u) ||
        ApuIsVirtualAddressValid(req.arrays[4], 350u * 64u + 1u))
        return false;

    auto* const owned = static_cast<uint8_t*>(req.arrays[4]);
    const uint32_t addr = ApuMapVirtualAddress(owned);
    if (!addr || ApuMapApuAddress(addr) != owned ||
        ApuMapApuAddress(addr + 350u * 64u - 1u) !=
            owned + 350u * 64u - 1u)
        return false;

    std::memset(owned, 0x5a, 350u * 64u);
    for (uint32_t i = 0; i < 350u * 64u; ++i)
        if (owned[i] != 0x5a)
            return false;

    if (allocate(&req) != S_OK || req.arrays[4] != owned ||
        host_apu_allocation_stats().live_allocations != 1)
        return false;
    ShapeContextRequest competing{};
    competing.counts[4] = 350;
    if (allocate(&competing) != HRESULT_FROM_WIN32(ERROR_BUSY) ||
        competing.arrays[4] ||
        ApuCreateHeap(16384, 32768) != HRESULT_FROM_WIN32(ERROR_BUSY))
        return false;

    if (release() != S_OK || req.arrays[4] != nullptr ||
        release() != S_OK ||
        ApuMapApuAddress(addr) != nullptr ||
        ApuMapVirtualAddress(owned) != 0)
        return false;
    const auto freed = host_apu_allocation_stats();
    if (freed.allocated_bytes != 0 || freed.live_allocations != 0)
        return false;

    if (allocate(&competing) != S_OK || !competing.arrays[4])
        return false;
    const uint32_t fresh = ApuMapVirtualAddress(competing.arrays[4]);
    if (!fresh || fresh == addr || ApuMapApuAddress(addr) != nullptr ||
        release() != S_OK || competing.arrays[4] != nullptr)
        return false;

    const auto final = host_apu_allocation_stats();
    return final.live_allocations == before.live_allocations &&
           final.allocated_bytes == before.allocated_bytes;
}
} // namespace scarlett::audio
