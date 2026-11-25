#include "xapu_host.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>

namespace scarlett::audio {
namespace {
using ConnectFn = HRESULT (WINAPI*)(
    const void*, const void*, HostXApuConnection**) noexcept;
using DisconnectFn = HRESULT (WINAPI*)(HostXApuConnection*) noexcept;
using DequeueFn = HRESULT (WINAPI*)(
    HostXApuConnection*, XApuResultEnvelope*) noexcept;
using CountFn = uint32_t (WINAPI*)(
    const HostXApuConnection*, uint32_t) noexcept;
static_assert(std::is_same_v<decltype(&XApuConnect), ConnectFn>);
static_assert(std::is_same_v<decltype(&XApuDisconnect), DisconnectFn>);
static_assert(std::is_same_v<decltype(&XApuDequeueResult), DequeueFn>);
static_assert(std::is_same_v<decltype(&XApuGetResultCount), CountFn>);

bool is_fixture_pattern(const XApuResultEnvelope& result,
                        uint32_t sequence) noexcept {
    for (uint32_t word = 0; word < 20; ++word)
        if (result.words[word] != (sequence ^ (word * 0x01010101u)))
            return false;
    return true;
}
XApuResultEnvelope generate_result(uint32_t sequence) noexcept {
    XApuResultEnvelope result{};
    for (uint32_t word = 0; word < 20; ++word)
        result.words[word] = sequence ^ (word * 0x01010101u);
    return result;
}
struct CanaryResult {
    uint64_t prefix = 0x1111111122222222ull;
    XApuResultEnvelope value{};
    uint64_t suffix = 0x3333333344444444ull;
};
} // namespace

bool verify_host_xapu_fixture() noexcept {
    try {
        const auto connected = find_xapu_export("XApuConnect");
        const auto disconnected = find_xapu_export("XApuDisconnect");
        const auto dequeued = find_xapu_export("XApuDequeueResult");
        const auto counted = find_xapu_export("XApuGetResultCount");
        if (connected != reinterpret_cast<FARPROC>(&XApuConnect) ||
            disconnected != reinterpret_cast<FARPROC>(&XApuDisconnect) ||
            dequeued != reinterpret_cast<FARPROC>(&XApuDequeueResult) ||
            counted != reinterpret_cast<FARPROC>(&XApuGetResultCount) ||
            find_xapu_export("XApuEnqueueCommand") !=
                reinterpret_cast<FARPROC>(&XApuEnqueueCommand) ||
            find_xapu_export("XApuConnectOrdinal") ||
            find_xapu_export("xapuconnect") ||
            find_xapu_export(""))
            return false;

        const auto connect = reinterpret_cast<ConnectFn>(connected);
        const auto disconnect = reinterpret_cast<DisconnectFn>(disconnected);
        const auto dequeue = reinterpret_cast<DequeueFn>(dequeued);
        const auto count = reinterpret_cast<CountFn>(counted);
        const uint64_t descriptor = 0x1234567890abcdefull;
        const uint64_t callbacks = 0xfedcba0987654321ull;
        const HostXApuStats before = host_xapu_stats();
        HostXApuConnection* sentinel =
            reinterpret_cast<HostXApuConnection*>(uintptr_t{0x1234});
        if (connect(&descriptor, &callbacks, nullptr) != E_POINTER ||
            connect(nullptr, &callbacks, &sentinel) != E_INVALIDARG ||
            sentinel ||
            connect(&descriptor, nullptr, &sentinel) != E_INVALIDARG ||
            sentinel)
            return false;

        HostXApuConnection* first = nullptr;
        HostXApuConnection* second = nullptr;
        if (connect(&descriptor, &callbacks, &first) != S_OK ||
            !first ||
            connect(&descriptor, &callbacks, &second) != S_OK ||
            !second || first == second)
            return false;

        HostXApuConnection* foreign =
            reinterpret_cast<HostXApuConnection*>(uintptr_t{0x99});
        CanaryResult buffer{};
        const XApuResultEnvelope empty_result{};
        if (count(nullptr, 0) || count(foreign, 0) ||
            count(first, 1) ||
            count(first, 0) || count(second, 0) ||
            dequeue(first, nullptr) != E_POINTER ||
            dequeue(first, &buffer.value) != kHostXApuPendingResults ||
            dequeue(foreign, &buffer.value) != E_INVALIDARG ||
            disconnect(foreign) != E_INVALIDARG ||
            buffer.prefix != 0x1111111122222222ull ||
            buffer.suffix != 0x3333333344444444ull ||
            std::memcmp(&buffer.value, &empty_result,
                        sizeof(buffer.value)) != 0)
            return false;

        for (uint32_t i = 0; i < kHostXApuResultCapacity; ++i) {
            if (!inject_synthetic_xapu_result_for_fixture(
                    first, generate_result(i + 1)))
                return false;
        }
        if (inject_synthetic_xapu_result_for_fixture(
                first, generate_result(0xffffffffu)) ||
            count(first, 0) != kHostXApuResultCapacity ||
            count(second, 0) != 0)
            return false;
        const auto queued = host_xapu_stats();
        if (queued.active_connections != before.active_connections + 2 ||
            queued.pending_results != before.pending_results +
                                        kHostXApuResultCapacity)
            return false;
        for (uint32_t i = 0; i < kHostXApuResultCapacity; ++i) {
            buffer.value = generate_result(0);
            if (dequeue(first, &buffer.value) != S_OK ||
                !is_fixture_pattern(buffer.value, i + 1) ||
                buffer.prefix != 0x1111111122222222ull ||
                buffer.suffix != 0x3333333344444444ull ||
                count(first, 0) != kHostXApuResultCapacity - i - 1)
                return false;
        }
        const XApuResultEnvelope unchanged = buffer.value;
        if (dequeue(first, &buffer.value) != kHostXApuPendingResults ||
            std::memcmp(&unchanged, &buffer.value, sizeof(unchanged)))
            return false;

        // Ring wraparound retains FIFO order.
        for (uint32_t i = 0; i < 100; ++i) {
            const auto source = generate_result(i + 0x1000u);
            if (!inject_synthetic_xapu_result_for_fixture(first, source) ||
                count(first, 0) != 1 ||
                dequeue(first, &buffer.value) != S_OK ||
                !is_fixture_pattern(buffer.value, i + 0x1000u))
                return false;
        }

        if (!inject_synthetic_xapu_result_for_fixture(
                first, generate_result(42)) ||
            disconnect(first) != S_OK ||
            disconnect(first) != E_INVALIDARG ||
            count(first, 0) != 0 ||
            inject_synthetic_xapu_result_for_fixture(
                first, generate_result(44)) ||
            dequeue(first, &buffer.value) != E_INVALIDARG ||
            count(second, 0) != 0)
            return false;

        std::atomic<bool> passed{true};
        std::vector<std::jthread> threads;
        threads.reserve(4);
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (uint32_t i = 0; i < 8; ++i) {
                    HostXApuConnection* connection = nullptr;
                    if (connect(&descriptor, &callbacks, &connection)
                            != S_OK || !connection) {
                        passed.store(false);
                        return;
                    }
                    const auto sample = generate_result(
                        5000u + uint32_t(t) * 100u + i);
                    XApuResultEnvelope result{};
                    if (!inject_synthetic_xapu_result_for_fixture(
                            connection, sample) ||
                        count(connection, 0) != 1 ||
                        dequeue(connection, &result) != S_OK ||
                        std::memcmp(&sample, &result, sizeof(sample)) ||
                        count(connection, 0) != 0 ||
                        disconnect(connection) != S_OK ||
                        count(connection, 0) != 0) {
                        passed.store(false);
                        return;
                    }
                }
            });
        }
        for (auto& thread : threads)
            thread.join();
        if (disconnect(second) != S_OK)
            return false;
        const auto after = host_xapu_stats();
        return passed.load() &&
               after.active_connections == before.active_connections &&
               after.pending_results == before.pending_results &&
               after.total_connections ==
                   before.total_connections + 34u;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::audio
