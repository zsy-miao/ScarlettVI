#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstddef>

namespace scarlett::xmem {
// The layout is derived independently by disassembling the user's
// XboxBvhBuilder.dll call sites, NOT from a restricted Xbox SDK.
// Win32 MEMORY_BASIC_INFORMATION fields occupy bytes 0..47.
// Byte 0x30 is read with mask 0x3 by the bundled library but its Xbox
// meaning is not publicly documented. This implementation uses a clearly
// designated desktop compatibility policy for those bits.
struct alignas(8) VirtualQueryInfo {
    MEMORY_BASIC_INFORMATION basic{};
    uint64_t desktop_access_flags = 0;
    uint64_t reserved = 0;
};
static_assert(sizeof(MEMORY_BASIC_INFORMATION) == 48);
static_assert(sizeof(VirtualQueryInfo) == 64);
static_assert(offsetof(VirtualQueryInfo, desktop_access_flags) == 0x30);

// The DLL passes rcx=GetCurrentProcess(), rdx=address, r8=64-byte info.
// No fourth argument is used in the examined call sites.
// This prototype is experimental and only intended for the caller patterns
// actually observed in the supplied BVH builder.
extern "C" BOOL WINAPI XMemVirtualQuery(HANDLE process, const void* address,
                                        VirtualQueryInfo* output);
} // namespace scarlett::xmem
