#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstddef>
#include <guiddef.h>

namespace scarlett::gpu {

// Xbox D3D12 device factory: experimental host COM compatibility subset.
// Only the observed 4-argument x64 ABI is supported. The returned interface
// is a genuine Windows ID3D12Device11, not a console D3D12_XS implementation.
struct XboxDeviceParametersObserved {
    uint32_t sdk_version = 0;
    uint32_t field_04 = 0;
    uint32_t graphics_queue_ring_bytes = 0;
    uint8_t unknown_0c_through_4f[0x50 - 12]{};
};
static_assert(sizeof(XboxDeviceParametersObserved) == 0x50);
static_assert(offsetof(XboxDeviceParametersObserved, graphics_queue_ring_bytes) == 8);

// Do not claim Xbox-only flags or memory policies are honored. This bridge
// can be useful for standard COM methods but is not sufficient for title boot.
// The supplied title zero-initializes exactly 32 bytes, then writes a
// 32-bit mode at offset 4 (observed values: 3 or 4). Other semantics are
// unknown; we accept only that demonstrated subset.
struct XboxGpuVaConfigurationObserved {
    uint32_t unused_00 = 0;
    uint32_t mode_04 = 0;
    uint32_t other_08_through_1c[6]{};
};
static_assert(sizeof(XboxGpuVaConfigurationObserved) == 32);
static_assert(offsetof(XboxGpuVaConfigurationObserved, mode_04) == 4);

// Reserves two real host D3D12 GPU virtual resource windows for the observed
// mode (3->16GiB, 4->24GiB). Does not map Xbox physical GPU pages or reproduce
// driver-specific console GPU VA rules.
HRESULT WINAPI xbox_configure_virtual_memory_host(
    const XboxGpuVaConfigurationObserved* configuration) noexcept;
bool verify_xbox_gpu_virtual_memory_configuration(uint32_t mode) noexcept;

HRESULT WINAPI xbox_create_device_host(
    void* xbox_adapter,
    const XboxDeviceParametersObserved* parameters,
    REFIID requested_interface,
    void** output) noexcept;

// Pure original host-side fixture. Exercises the actual Xbox ABI-shaped
// call, returns a real COM device and verifies failure/ownership behavior.
bool verify_xbox_device_factory() noexcept;
} // namespace scarlett::gpu
