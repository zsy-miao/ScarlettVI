#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>

namespace scarlett::gpu {

// Fail-closed compatibility entry points for the two public XG XS imports.
// Windows x64 uses one calling convention: the pointer/flag arguments
// match the published Microsoft XG call shapes, but the Xbox layout and
// returned texture-computer object ABI are NOT emulated.
// In particular, these functions never dereference or write an unknown
// guest descriptor or output pointer; they never report fake success.
HRESULT WINAPI xg_create_texture_computer_host(
    const void* resource_desc, void** texture_computer) noexcept;
HRESULT WINAPI xg_suggest_swizzle_mode_host(
    const void* resource_desc, uint32_t flags,
    void* texture_layout) noexcept;

FARPROC find_xg_xs_error_only_export(const std::string& name) noexcept;
bool verify_xg_xs_error_only_fixture() noexcept;
} // namespace scarlett::gpu
