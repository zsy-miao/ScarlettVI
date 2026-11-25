#pragma once
#include <cstdint>

namespace scarlett::gpu {

// D3D12 documentation: ID3D12Fence::GetCompletedValue() returns UINT64_MAX
// when the device has been removed. A naive >= fence-value check would then
// incorrectly classify the GPU as completed and release live page mappings.
enum class FenceObservation {
    pending,
    completed,
    device_removed
};
constexpr FenceObservation classify_fence_observation(
    uint64_t observed, uint64_t expected) noexcept {
    if (observed == UINT64_MAX) return FenceObservation::device_removed;
    if (observed >= expected) return FenceObservation::completed;
    return FenceObservation::pending;
}
static_assert(classify_fence_observation(0, 1) == FenceObservation::pending);
static_assert(classify_fence_observation(3, 3) == FenceObservation::completed);
static_assert(classify_fence_observation(4, 3) == FenceObservation::completed);
static_assert(classify_fence_observation(UINT64_MAX, 3) ==
              FenceObservation::device_removed);
} // namespace scarlett::gpu
