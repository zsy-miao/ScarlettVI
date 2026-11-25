#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace scarlett::audio {

struct HostXApuConnection;

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
uint32_t WINAPI XApuGetResultCount(
    const HostXApuConnection* connection, uint32_t selector) noexcept;
}

// Original partial host-backed connection and result queue. EnqueueCommand
// intentionally remains unbound: it would require actual verified Xbox
// command processing. A fixture-only result injection tests the queue
// without pretending any guest command executed.
FARPROC find_xapu_export(std::string_view name) noexcept;
HostXApuStats host_xapu_stats() noexcept;
bool inject_synthetic_xapu_result_for_fixture(
    HostXApuConnection* connection, const XApuResultEnvelope& result) noexcept;
bool verify_host_xapu_fixture() noexcept;
} // namespace scarlett::audio
