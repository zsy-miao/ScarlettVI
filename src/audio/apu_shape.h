#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace scarlett::audio {
// The inspected title uses six 32-bit counts followed by six 64-bit
// output pointers. Only family 4's 64-byte record stride is verified.
// All other nonempty families are rejected, not guessed or silently filled.
struct ShapeContextRequest {
    uint32_t counts[6]{};
    void* arrays[6]{};
};
static_assert(offsetof(ShapeContextRequest, arrays) == 0x18);
static_assert(sizeof(ShapeContextRequest) == 0x48);

inline constexpr uint32_t kHostMaxShapeContexts = 4096;
inline constexpr uint32_t kHostShapeRecordBytes = 64;

extern "C" {
HRESULT WINAPI AcpHalAllocateShapeContexts(
    ShapeContextRequest* request) noexcept;
// The inspected title invokes this without any arguments and ignores RAX.
HRESULT WINAPI AcpHalReleaseShapeContexts() noexcept;
}

FARPROC find_shape_export(std::string_view name) noexcept;
bool verify_apu_shape_fixtures() noexcept;
} // namespace scarlett::audio
