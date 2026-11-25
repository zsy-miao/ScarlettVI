#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace scarlett::audio {

struct HostXApuConnection;

// ScarlettVI-generated host command, not an Xbox XAPU command schema.
// This exact shape is recognized ONLY when the entire command resides
// in a mapped XMem CPU alias; unknown Xbox commands remain E_NOTIMPL.
inline constexpr uint32_t kHostXApuFirCommandMagic = 0x31464953u; // "SIF1"
struct HostXApuFirCommandV1 {
    uint32_t magic = kHostXApuFirCommandMagic;
    uint16_t version = 1;
    uint16_t reserved = 0;
    const float* input = nullptr;
    float* output = nullptr;
    uint32_t samples = 0;
    uint32_t reserved2 = 0;
    uint64_t command_id = 0;
    uint64_t reserved3 = 0;
};
static_assert(sizeof(HostXApuFirCommandV1) == 48);
static_assert(offsetof(HostXApuFirCommandV1, input) == 8);
static_assert(offsetof(HostXApuFirCommandV1, output) == 16);
static_assert(offsetof(HostXApuFirCommandV1, command_id) == 32);

// Title's one directly observed dequeue caller zeroes an 0x50-byte
// output buffer and reads fields at +0, +4, and +0x34. No completion
// hardware contract is implied by this host-owned result envelope.
struct XApuResultEnvelope {
    uint32_t words[20]{};
};
static_assert(sizeof(XApuResultEnvelope) == 0x50);

inline constexpr size_t kHostXApuMaxConnections = 64;
inline constexpr size_t kHostXApuResultCapacity = 64;
inline constexpr HRESULT kHostXApuPendingResults =
    static_cast<HRESULT>(0x8AC81001u); // public Xbox GDK code
inline constexpr HRESULT kHostXApuQueueFull =
    static_cast<HRESULT>(0x8AC81000u); // public Xbox GDK code
// Internal host-generated FIR receipt, NOT a guest/Xbox result ID.
inline constexpr uint32_t kHostXApuFirResultMagic = 0x52494653u; // "SFIR"

struct HostXApuStats {
    size_t total_connections = 0;
    size_t active_connections = 0;
    size_t pending_results = 0;
};

extern "C" {
HRESULT WINAPI XApuConnect(
    const void* connection_descriptor, const void* callbacks,
    HostXApuConnection** output) noexcept;
HRESULT WINAPI XApuDequeueResult(HostXApuConnection* connection,
                                XApuResultEnvelope* output) noexcept;
HRESULT WINAPI XApuDisconnect(HostXApuConnection* connection) noexcept;
// Strict host-only FIR command shape; unsupported Xbox commands are
// never decoded as success and never trigger false DSP completions.
HRESULT WINAPI XApuEnqueueCommand(
    HostXApuConnection* connection,
    const void* command, uint32_t flags) noexcept;
uint32_t WINAPI XApuGetResultCount(
    const HostXApuConnection* connection, uint32_t selector) noexcept;
}

// Original partial host-backed connection and result queue. The
// EnqueueCommand import now recognizes ONLY an explicitly marked,
// XMem-resident, ScarlettVI-generated FIR command, executes true host
// DSP and produces a real host receipt. Xbox command decoding is still
// unsupported: all unrecognized commands return E_NOTIMPL, never a
// fabricated completion. Typed host FIR operations remain available.
FARPROC find_xapu_export(std::string_view name) noexcept;
HostXApuStats host_xapu_stats() noexcept;
HRESULT configure_host_xapu_fir(
    HostXApuConnection* connection, std::span<const float> kernel,
    uint32_t channels) noexcept;
HRESULT submit_host_xapu_fir(
    HostXApuConnection* connection, std::span<const float> input,
    std::span<float> output, uint64_t command_id) noexcept;

// Fixture-only injection of host-generated test receipts.
bool inject_synthetic_xapu_result_for_fixture(
    HostXApuConnection* connection, const XApuResultEnvelope& result) noexcept;
bool verify_host_xapu_fixture() noexcept;
bool verify_host_xapu_dsp_integration_fixture() noexcept;
bool verify_host_xapu_guest_fir_fixture() noexcept;
} // namespace scarlett::audio
