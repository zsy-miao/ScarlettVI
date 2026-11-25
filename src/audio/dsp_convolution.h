#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace scarlett::audio {

// Original Windows CPU finite impulse response (FIR) engine. Real signal
// processing for synthetic float samples only; NOT an Xbox XDsp ABI adapter.
// A per-channel circular history handles chunk boundaries and arbitrary
// interleaved mono/stereo/multichannel buffer lengths.
class HostDspConvolver final {
public:
    static constexpr uint32_t kMaxChannels = 8;
    static constexpr size_t kMaxTaps = 4096;
    static constexpr size_t kMaxFramesPerCall = 4096;

    // Transactional: invalid/unallocatable configuration leaves the
    // previously installed kernel and history unchanged.
    HRESULT configure(std::span<const float> kernel,
                      uint32_t channels) noexcept;

    // Transactional: invalid input leaves output, state, and frame count
    // unchanged. In-place interleaved processing is supported.
    HRESULT process(std::span<const float> input,
                    std::span<float> output) noexcept;

    // Clear stream history while retaining the installed kernel.
    HRESULT reset() noexcept;
    uint64_t frames_processed() const noexcept;
    uint32_t channels() const noexcept;
    size_t tap_count() const noexcept;

private:
    mutable std::mutex mutex_;
    uint32_t channels_ = 0;
    std::vector<float> kernel_;
    std::vector<float> history_;
    size_t head_ = 0;
    uint64_t frames_processed_ = 0;
};

bool verify_host_dsp_convolution_fixture() noexcept;

} // namespace scarlett::audio
