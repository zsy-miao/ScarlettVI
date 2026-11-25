#include "xapu_host.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace scarlett::audio {
namespace {
uint32_t checksum_of(std::span<const float> values) noexcept {
    uint32_t checksum = 2166136261u;
    for (float sample : values) {
        checksum ^= std::bit_cast<uint32_t>(sample);
        checksum *= 16777619u;
    }
    return checksum;
}
bool verify_receipt(const XApuResultEnvelope& result,
                    uint64_t id, uint32_t frames, uint32_t channels,
                    uint64_t cumulative, uint32_t checksum) noexcept {
    if (result.words[0] != kHostXApuFirResultMagic ||
        result.words[1] != static_cast<uint32_t>(id) ||
        result.words[2] != static_cast<uint32_t>(id >> 32) ||
        result.words[3] != frames || result.words[4] != channels ||
        result.words[5] != checksum ||
        result.words[6] != static_cast<uint32_t>(cumulative) ||
        result.words[7] != static_cast<uint32_t>(cumulative >> 32))
        return false;
    for (size_t i = 8; i < 20; ++i)
        if (result.words[i] != 0)
            return false;
    return true;
}
bool floats_equal(std::span<const float> a,
                  std::span<const float> b) noexcept {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}
} // namespace

bool verify_host_xapu_dsp_integration_fixture() noexcept {
    try {
        // All calls below are internal typed host operations. There is
        // deliberately NO guest XApuEnqueueCommand export.
        if (find_xapu_export("XApuEnqueueCommand"))
            return false;
        uint64_t opaque_desc = 0x1234;
        uint64_t opaque_callbacks = 0x9876;
        const HostXApuStats before = host_xapu_stats();
        HostXApuConnection* connection = nullptr;
        if (XApuConnect(&opaque_desc, &opaque_callbacks, &connection)
                != S_OK || !connection)
            return false;

        const std::array<float,3> taps{1.0f, 0.5f, 0.25f};
        const std::array<float,4> stereo_impulse{1.0f, 2.0f, 0.0f, 0.0f};
        const std::array<float,4> stereo_expected{1.0f, 2.0f, 0.5f, 1.0f};
        std::array<float,4> output{};
        // Reject submitting before an actual DSP filter was configured.
        if (submit_host_xapu_fir(connection, stereo_impulse, output, 1)
                != E_NOT_VALID_STATE ||
            XApuGetResultCount(connection, 0) != 0 ||
            configure_host_xapu_fir(connection, taps, 0) != E_INVALIDARG ||
            configure_host_xapu_fir(connection, taps, 2) != S_OK)
            return false;

        constexpr uint64_t kFirstId = 0x12345678ABCDEF01ull;
        if (submit_host_xapu_fir(connection, stereo_impulse,
                                 output, kFirstId) != S_OK ||
            !floats_equal(output, stereo_expected) ||
            XApuGetResultCount(connection, 0) != 1 ||
            configure_host_xapu_fir(connection, taps, 2) !=
                HRESULT_FROM_WIN32(ERROR_BUSY))
            return false;
        XApuResultEnvelope result{};
        if (XApuDequeueResult(connection, &result) != S_OK ||
            !verify_receipt(result, kFirstId, 2, 2, 2,
                            checksum_of(output)) ||
            XApuGetResultCount(connection, 0) != 0)
            return false;

        // Streaming across separate host commands must preserve prior
        // left and right input histories, and completions must be FIFO.
        const std::array<float,4> zeros{};
        const std::array<float,4> tail_expected{0.25f, 0.5f, 0, 0};
        std::array<float,4> tail_output{};
        if (submit_host_xapu_fir(connection, zeros,
                                 tail_output, 2) != S_OK ||
            !floats_equal(tail_output, tail_expected) ||
            XApuDequeueResult(connection, &result) != S_OK ||
            !verify_receipt(result, 2, 2, 2, 4,
                            checksum_of(tail_output)))
            return false;

        // Reconfigure resets history only after outstanding completions
        // have been consumed. Invalid kernel keeps existing configuration.
        const std::array<float,2> bad_taps{
            1.0f, std::numeric_limits<float>::quiet_NaN()};
        if (configure_host_xapu_fir(connection, bad_taps, 2)
                != E_INVALIDARG ||
            configure_host_xapu_fir(connection, taps, 2) != S_OK)
            return false;

        // Failed non-finite sample processing must not generate receipts
        // or write to the output.
        const std::array<float,4> invalid_input{
            0, std::numeric_limits<float>::infinity(), 0, 0};
        std::array<float,4> canary{77, 78, 79, 80};
        if (submit_host_xapu_fir(connection, invalid_input,
                                 canary, 3) != E_INVALIDARG ||
            !floats_equal(canary,
                          std::array<float,4>{77,78,79,80}) ||
            XApuGetResultCount(connection, 0) != 0)
            return false;

        // Fill the actual host completion FIFO. A full queue must reject
        // a real DSP call before changing output or filter history.
        for (uint32_t i = 0; i < kHostXApuResultCapacity; ++i) {
            XApuResultEnvelope synthetic{};
            synthetic.words[0] = i + 1;
            if (!inject_synthetic_xapu_result_for_fixture(
                    connection, synthetic))
                return false;
        }
        canary = {77,78,79,80};
        if (submit_host_xapu_fir(connection, stereo_impulse, canary, 4)
                != kHostXApuQueueFull ||
            !floats_equal(canary,
                          std::array<float,4>{77,78,79,80}) ||
            XApuGetResultCount(connection, 0) != kHostXApuResultCapacity)
            return false;
        for (uint32_t i = 0; i < kHostXApuResultCapacity; ++i) {
            if (XApuDequeueResult(connection, &result) != S_OK ||
                result.words[0] != i + 1)
                return false;
        }
        if (submit_host_xapu_fir(connection, stereo_impulse,
                                 output, 5) != S_OK ||
            !floats_equal(output, stereo_expected) ||
            XApuDequeueResult(connection, &result) != S_OK ||
            !verify_receipt(result, 5, 2, 2, 2,
                            checksum_of(output)))
            return false;

        // In-place processing must produce actual processed samples,
        // and the recorded checksum must reflect those samples.
        if (configure_host_xapu_fir(connection, taps, 2) != S_OK)
            return false;
        std::array<float,4> in_place = stereo_impulse;
        if (submit_host_xapu_fir(connection, in_place,
                                 in_place, 6) != S_OK ||
            !floats_equal(in_place, stereo_expected) ||
            XApuDequeueResult(connection, &result) != S_OK ||
            !verify_receipt(result, 6, 2, 2, 2,
                            checksum_of(in_place)))
            return false;

        const auto foreign = reinterpret_cast<HostXApuConnection*>(
            uintptr_t{0x12345});
        std::array<float,4> rejected{10, 11, 12, 13};
        if (configure_host_xapu_fir(foreign, taps, 2) != E_INVALIDARG ||
            submit_host_xapu_fir(foreign, stereo_impulse,
                                 rejected, 7) != E_INVALIDARG ||
            !floats_equal(rejected,
                          std::array<float,4>{10,11,12,13}))
            return false;

        // Four producers submit real identity-FIR work concurrently
        // through one shared handle. The queue must contain each unique
        // completed command ID exactly once, with exact output checksums.
        HostXApuConnection* shared = nullptr;
        if (XApuConnect(&opaque_desc, &opaque_callbacks, &shared)
                != S_OK || !shared)
            return false;
        const std::array<float,1> identity{1.0f};
        if (configure_host_xapu_fir(shared, identity, 1) != S_OK)
            return false;
        std::atomic<bool> passed{true};
        std::vector<std::jthread> workers;
        workers.reserve(4);
        for (uint32_t thread = 0; thread < 4; ++thread) {
            workers.emplace_back([&, thread] {
                for (uint32_t i = 0; i < 8; ++i) {
                    const std::array<float,2> samples{
                        static_cast<float>(thread * 100 + i),
                        static_cast<float>(thread * 100 + i + 1)};
                    std::array<float,2> processed{};
                    const uint64_t id = thread * 8u + i + 1u;
                    if (submit_host_xapu_fir(
                            shared, samples, processed, id) != S_OK ||
                        !floats_equal(samples, processed))
                        passed.store(false);
                }
            });
        }
        for (auto& worker : workers)
            worker.join();
        if (!passed.load() || XApuGetResultCount(shared, 0) != 32)
            return false;
        std::array<bool,33> seen{};
        for (uint32_t i = 0; i < 32; ++i) {
            if (XApuDequeueResult(shared, &result) != S_OK ||
                result.words[0] != kHostXApuFirResultMagic ||
                result.words[2] != 0 ||
                result.words[1] == 0 || result.words[1] > 32 ||
                seen[result.words[1]] ||
                result.words[3] != 2 || result.words[4] != 1 ||
                result.words[6] != 2 * (i + 1))
                return false;
            seen[result.words[1]] = true;
            const uint32_t id = result.words[1] - 1;
            const uint32_t thread = id / 8;
            const uint32_t index = id % 8;
            const std::array<float,2> expected{
                static_cast<float>(thread * 100 + index),
                static_cast<float>(thread * 100 + index + 1)};
            if (result.words[5] != checksum_of(expected))
                return false;
        }
        for (uint32_t i = 1; i <= 32; ++i)
            if (!seen[i])
                return false;

        if (XApuDisconnect(shared) != S_OK ||
            XApuDisconnect(connection) != S_OK ||
            configure_host_xapu_fir(connection, taps, 2)
                != E_INVALIDARG ||
            submit_host_xapu_fir(connection, stereo_impulse,
                                 rejected, 8) != E_INVALIDARG)
            return false;
        const auto after = host_xapu_stats();
        return after.active_connections == before.active_connections &&
               after.pending_results == before.pending_results &&
               after.total_connections == before.total_connections + 2u;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::audio
