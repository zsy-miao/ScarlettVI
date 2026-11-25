#include "dsp_convolution.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace scarlett::audio {
namespace {
bool finite_float_sequence(std::span<const float> values) noexcept {
    for (float value : values)
        if (!std::isfinite(value))
            return false;
    return true;
}
} // namespace

HRESULT HostDspConvolver::configure(std::span<const float> taps,
                                   uint32_t channels) noexcept {
    if (!channels || channels > kMaxChannels ||
        taps.empty() || taps.size() > kMaxTaps ||
        !finite_float_sequence(taps))
        return E_INVALIDARG;

    try {
        // Allocate before taking the lock, so allocation failures cannot
        // destroy an existing functioning stream configuration.
        std::vector<float> kernel(taps.begin(), taps.end());
        std::vector<float> history(taps.size() * channels, 0.0f);
        const std::lock_guard lock(mutex_);
        kernel_.swap(kernel);
        history_.swap(history);
        channels_ = channels;
        head_ = 0;
        frames_processed_ = 0;
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HRESULT HostDspConvolver::process(
    std::span<const float> input, std::span<float> output) noexcept {
    const std::lock_guard lock(mutex_);
    if (channels_ == 0)
        return E_NOT_VALID_STATE;
    if (input.empty() || input.size() % channels_ ||
        input.size() / channels_ > kMaxFramesPerCall ||
        output.size() < input.size() || !finite_float_sequence(input))
        return E_INVALIDARG;

    const size_t frames = input.size() / channels_;
    const size_t taps = kernel_.size();
    if (frames_processed_ > std::numeric_limits<uint64_t>::max() - frames)
        return E_INVALIDARG;

    try {
        // Compute against private copies; no caller data or stream state
        // changes until every output has been calculated successfully.
        // This also supports safely processing input==output.
        auto scratch_history = history_;
        std::vector<float> scratch_output(input.size(), 0);
        size_t head = head_;
        for (size_t frame = 0; frame < frames; ++frame) {
            for (uint32_t ch = 0; ch < channels_; ++ch)
                scratch_history[ch * taps + head] =
                    input[frame * channels_ + ch];
            for (uint32_t ch = 0; ch < channels_; ++ch) {
                const float* channel_history =
                    scratch_history.data() + ch * taps;
                double sum = 0;
                size_t history_index = head;
                for (size_t tap = 0; tap < taps; ++tap) {
                    sum += static_cast<double>(kernel_[tap]) *
                           static_cast<double>(
                               channel_history[history_index]);
                    // No division/modulus in the per-tap DSP hot loop.
                    history_index = history_index == 0
                        ? taps - 1 : history_index - 1;
                }
                // Do not silently return infinities to an audio consumer.
                if (!std::isfinite(sum) ||
                    std::abs(sum) >
                        static_cast<double>(std::numeric_limits<float>::max()))
                    return E_INVALIDARG;
                scratch_output[frame * channels_ + ch] =
                    static_cast<float>(sum);
            }
            head = (head + 1) % taps;
        }
        history_.swap(scratch_history);
        head_ = head;
        frames_processed_ += frames;
        std::copy(scratch_output.begin(), scratch_output.end(),
                  output.begin());
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HRESULT HostDspConvolver::reset() noexcept {
    const std::lock_guard lock(mutex_);
    if (channels_ == 0)
        return E_NOT_VALID_STATE;
    std::fill(history_.begin(), history_.end(), 0.0f);
    head_ = 0;
    frames_processed_ = 0;
    return S_OK;
}

uint64_t HostDspConvolver::frames_processed() const noexcept {
    const std::lock_guard lock(mutex_);
    return frames_processed_;
}
uint32_t HostDspConvolver::channels() const noexcept {
    const std::lock_guard lock(mutex_);
    return channels_;
}
size_t HostDspConvolver::tap_count() const noexcept {
    const std::lock_guard lock(mutex_);
    return kernel_.size();
}
} // namespace scarlett::audio
