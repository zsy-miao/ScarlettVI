#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace scarlett::audio {

struct HostXDspClient;

// Host-created command, never an Xbox XDsp command descriptor.
// The actual console packet ABI is not yet known. Only this magic,
// version, and XMem-backed payload may reach the CPU FIR processor.
inline constexpr uint32_t kHostXDspFirCommandMagic = 0x31534448u; // "HDS1"
struct HostXDspFirCommandV1 {
    uint32_t magic = kHostXDspFirCommandMagic;
    uint16_t version = 1;
    uint16_t reserved = 0;
    const float* input = nullptr;
    float* output = nullptr;
    uint32_t samples = 0;
    uint32_t reserved2 = 0;
    uint64_t command_id = 0;
    uint64_t reserved3 = 0;
};
static_assert(sizeof(HostXDspFirCommandV1) == 48);
static_assert(offsetof(HostXDspFirCommandV1, input) == 8);
static_assert(offsetof(HostXDspFirCommandV1, output) == 16);
static_assert(offsetof(HostXDspFirCommandV1, command_id) == 32);

inline constexpr size_t kMaxHostXDspClients = 64;

// The six-argument Connect and two-argument Submit shape are inferred
// from read-only title callsites. Opaque descriptors are NEVER read.
// The signatures of Activate/Deactivate and the actual stream contract
// remain provisional, so those functions never use their parameters.
extern "C" {
HRESULT WINAPI XDspConnectWithMaximumStreamLimit(
    const void* configuration, uint32_t stream_hint,
    uint32_t maximum_streams, uint32_t maximum_impulse,
    uint32_t flags, HostXDspClient** output) noexcept;
HRESULT WINAPI XDspDisconnect(HostXDspClient* client) noexcept;
// Five opaque argument positions were observed in an Activate wrapper;
// unknown data never gets inspected and no stream is fabricated.
HRESULT WINAPI XDspActivate(void* opaque_a, void* opaque_b,
                            uintptr_t opaque_c, uintptr_t opaque_d,
                            uintptr_t opaque_e) noexcept;
HRESULT WINAPI XDspDeactivate(void* opaque_stream) noexcept;
HRESULT WINAPI XDspSubmitCommand(
    HostXDspClient* client, const void* command) noexcept;
}

struct HostXDspStats {
    size_t registered_clients = 0;
    size_t live_clients = 0;
    uint64_t host_fir_commands = 0;
};

// Host-generated test setup, not an Xbox import. Configures actual FIR
// processing for the tagged XMem command path, without claiming
// Xbox activation or playback.
HRESULT configure_host_xdsp_fir(
    HostXDspClient* client, std::span<const float> taps,
    uint32_t channels) noexcept;
HostXDspStats host_xdsp_stats() noexcept;
FARPROC find_chad_export(std::string_view name) noexcept;
bool verify_chad_host_fixture() noexcept;

} // namespace scarlett::audio
