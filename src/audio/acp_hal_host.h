#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace scarlett::audio {

struct HostHalObject;

// The guest has six pointer-sized slots through +0x28. Only four slots
// were observed in the inspected title. We intentionally do not invent
// signatures or implementations for +0x18 and +0x20.
struct HostHalVtable {
    HRESULT (WINAPI* start)(HostHalObject*, uint32_t, uint32_t) noexcept;
    HRESULT (WINAPI* stop)(HostHalObject*) noexcept;
    HRESULT (WINAPI* configure)(HostHalObject*, uint32_t, uint32_t,
                                uint32_t, const uint32_t*, uint32_t) noexcept;
    void* unverified18;
    void* unverified20;
    HRESULT (WINAPI* destroy)(HostHalObject*) noexcept;
};
static_assert(sizeof(HostHalVtable) == 0x30);
static_assert(offsetof(HostHalVtable, configure) == 0x10);
static_assert(offsetof(HostHalVtable, destroy) == 0x28);

struct HostHalObject {
    const HostHalVtable* vtable;
};
static_assert(offsetof(HostHalObject, vtable) == 0);

struct HostHalStats {
    size_t live_objects;
    size_t live_buffers;
    uint64_t live_buffer_bytes;
};

struct HostHalBuffers {
    const uint8_t* input = nullptr;
    const uint8_t* output = nullptr;
    uint32_t input_bytes = 0;
    uint32_t output_bytes = 0;
};

// Original, limited host-only HAL queue allocation object.
// The third vtable method is *deliberately unsupported* (E_NOTIMPL):
// a non-null import address is not an implemented DSP endpoint.
extern "C" HRESULT WINAPI AcpHalCreate(HostHalObject** output) noexcept;
FARPROC find_acphal_factory_export(std::string_view symbol) noexcept;
HostHalStats host_hal_stats() noexcept;
bool inspect_host_hal_buffers_for_fixture(const HostHalObject* object,
                                         HostHalBuffers* out) noexcept;
bool verify_host_hal_factory_fixture() noexcept;

} // namespace scarlett::audio
