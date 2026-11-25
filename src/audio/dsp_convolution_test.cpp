#include "dsp_convolution.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

namespace scarlett::audio {
namespace {
bool equal_buffers(std::span<const float> actual,
                   std::span<const float> expected) noexcept {
    if (actual.size() != expected.size()) return false;
    for (size_t i = 0; i < actual.size(); ++i)
        if (actual[i] != expected[i]) return false;
    return true;
}
} // namespace

bool verify_host_dsp_convolution_fixture() noexcept {
    try {
        HostDspConvolver fir;
        std::array<float, 6> initial_output{};
        const std::array<float, 6> unit_impulse{1, 0, 0, 0, 0, 0};
        if (fir.process(unit_impulse, initial_output) != E_NOT_VALID_STATE ||
            fir.reset() != E_NOT_VALID_STATE ||
            fir.channels() != 0 || fir.frames_processed() != 0)
            return false;

        constexpr std::array<float, 3> kernel{1.0f, 0.5f, 0.25f};
        constexpr std::array<float, 6> expected{
            1.0f, 0.5f, 0.25f, 0.0f, 0.0f, 0.0f};
        if (fir.configure(kernel, 1) != S_OK ||
            fir.channels() != 1 || fir.tap_count() != 3 ||
            fir.process(unit_impulse, initial_output) != S_OK ||
            !equal_buffers(initial_output, expected) ||
            fir.frames_processed() != 6)
            return false;

        const std::array<float, 3> invalid_kernel{
            1.0f, std::numeric_limits<float>::quiet_NaN(), 0.25f};
        const std::array<float, 1> infinite_kernel{
            std::numeric_limits<float>::infinity()};
        std::vector<float> huge_kernel(
            HostDspConvolver::kMaxTaps + 1, 1.0f);
        if (fir.configure(std::span<const float>{}, 1) != E_INVALIDARG ||
            fir.configure(kernel, 0) != E_INVALIDARG ||
            fir.configure(kernel, 9) != E_INVALIDARG ||
            fir.configure(invalid_kernel, 1) != E_INVALIDARG ||
            fir.configure(infinite_kernel, 1) != E_INVALIDARG ||
            fir.configure(huge_kernel, 1) != E_INVALIDARG ||
            fir.tap_count() != 3 || fir.frames_processed() != 6)
            return false;

        // On a poisoned input, output and state must not change.
        const std::array<float, 3> poisoned{
            0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f};
        std::array<float, 3> untouched{42.0f, 43.0f, 44.0f};
        if (fir.process(poisoned, untouched) != E_INVALIDARG ||
            !equal_buffers(untouched,
                           std::array<float, 3>{42.0f,43.0f,44.0f}) ||
            fir.frames_processed() != 6)
            return false;

        // The previous result history persists across separate chunk calls,
        // and reset restores the exact initialized stream state.
        if (fir.reset() != S_OK ||
            fir.frames_processed() != 0)
            return false;
        std::array<float, 6> in_place = unit_impulse;
        if (fir.process(in_place, in_place) != S_OK ||
            !equal_buffers(in_place, expected))
            return false;

        if (fir.reset() != S_OK)
            return false;
        std::array<float, 2> chunk1{1.0f, 0.0f};
        std::array<float, 4> chunk2{0.0f, 0.0f, 0.0f, 0.0f};
        if (fir.process(std::span<const float>{chunk1}, chunk1) != S_OK ||
            fir.process(std::span<const float>{chunk2}, chunk2) != S_OK ||
            !equal_buffers(chunk1, std::array<float,2>{1.0f,0.5f}) ||
            !equal_buffers(chunk2,
                           std::array<float,4>{0.25f,0,0,0}) ||
            fir.frames_processed() != 6)
            return false;

        // Real interleaved channel separation: each channel keeps its own
        // history, and no samples bleed from left to right or vice versa.
        if (fir.configure(kernel, 2) != S_OK ||
            fir.channels() != 2 || fir.frames_processed() != 0)
            return false;
        const std::array<float, 8> stereo_input{
            1, 2, 0, 0, 0, 0, 0, 0};
        std::array<float, 8> stereo_output{};
        const std::array<float, 8> stereo_expected{
            1, 2, 0.5f, 1, 0.25f, 0.5f, 0, 0};
        if (fir.process(stereo_input, stereo_output) != S_OK ||
            !equal_buffers(stereo_output, stereo_expected) ||
            fir.frames_processed() != 4)
            return false;

        // Reject format/size errors without changing previously written
        // output values or the stream's frame count.
        const std::array<float, 3> bad_interleave{1, 2, 3};
        std::array<float, 3> canary{100, 101, 102};
        if (fir.process(bad_interleave, canary) != E_INVALIDARG ||
            fir.process(std::span<const float>{}, canary) != E_INVALIDARG ||
            fir.process(stereo_input, canary) != E_INVALIDARG ||
            !equal_buffers(canary, std::array<float,3>{100,101,102}) ||
            fir.frames_processed() != 4)
            return false;

        // Four thousand ninety-six actual stereo frames at maximum allowed
        // channel count, and a rejection just beyond the bounded call size.
        constexpr std::array<float, 1> identity{1.0f};
        if (fir.configure(identity, HostDspConvolver::kMaxChannels)
            != S_OK)
            return false;
        std::vector<float> full_input(
            HostDspConvolver::kMaxFramesPerCall *
            HostDspConvolver::kMaxChannels);
        for (size_t i = 0; i < full_input.size(); ++i)
            full_input[i] = static_cast<float>(static_cast<int>(i % 31) - 15);
        std::vector<float> full_output(full_input.size(), 0);
        if (fir.process(full_input, full_output) != S_OK ||
            !equal_buffers(full_input, full_output) ||
            fir.frames_processed() != HostDspConvolver::kMaxFramesPerCall)
            return false;
        full_input.resize(full_input.size() + fir.channels(), 0);
        full_output.resize(full_input.size(), 77);
        const uint64_t pre_invalid = fir.frames_processed();
        if (fir.process(full_input, full_output) != E_INVALIDARG ||
            fir.frames_processed() != pre_invalid ||
            full_output.back() != 77)
            return false;

        // Valid finite inputs producing an unrepresentable float must fail
        // transactionally: neither the output nor history is committed.
        constexpr std::array<float,1> large_tap{
            std::numeric_limits<float>::max()};
        constexpr std::array<float,1> large_input{
            std::numeric_limits<float>::max()};
        std::array<float,1> large_out{99.0f};
        if (fir.configure(large_tap, 1) != S_OK ||
            fir.process(large_input, large_out) != E_INVALIDARG ||
            large_out[0] != 99.0f || fir.frames_processed() != 0)
            return false;

        // Lock-stable state shared by four workers. With an identity FIR
        // kernel, results are exactly independent of processing order.
        HostDspConvolver shared;
        if (shared.configure(identity, 1) != S_OK)
            return false;
        std::atomic<bool> passed{true};
        std::vector<std::jthread> workers;
        workers.reserve(4);
        for (int thread = 0; thread < 4; ++thread) {
            workers.emplace_back([&, thread] {
                std::array<float,32> input{};
                std::array<float,32> output{};
                for (int i = 0; i < 128; ++i) {
                    for (size_t k = 0; k < input.size(); ++k)
                        input[k] = static_cast<float>(
                            thread * 1000 + i * 32 +
                            static_cast<int>(k));
                    if (shared.process(input, output) != S_OK ||
                        !equal_buffers(input, output)) {
                        passed.store(false);
                        return;
                    }
                }
            });
        }
        for (auto& worker : workers)
            worker.join();
        return passed.load() &&
               shared.frames_processed() == 4u * 128u * 32u &&
               shared.reset() == S_OK &&
               shared.frames_processed() == 0;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::audio
