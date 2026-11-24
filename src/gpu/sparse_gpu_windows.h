#pragma once
#include "guest_address_space.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

namespace scarlett::gpu {

// Reserves actual D3D12 GPU VIRTUAL address spans; no VRAM is committed.
// Windows reserved buffers have NO physical pages until tile mappings are
// installed. This does not implement Xbox D3D12ConfigureVirtualMemoryX.
class SparseGpuWindows final {
public:
    struct Window {
        uint64_t synthetic_guest_base = 0;
        uint64_t bytes = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> reserved;
    };
    SparseGpuWindows() = default;
    SparseGpuWindows(const SparseGpuWindows&) = delete;
    SparseGpuWindows& operator=(const SparseGpuWindows&) = delete;
    ~SparseGpuWindows();

    HRESULT initialize(ID3D12Device11* device, uint64_t window_bytes,
                       uint32_t window_count) noexcept;
    void clear() noexcept;
    size_t window_count() const noexcept { return windows_.size(); }
    uint32_t gpu_va_bits_per_process() const noexcept { return gpu_va_bits_; }
    uint32_t gpu_va_bits_per_resource() const noexcept { return resource_bits_; }
    uint64_t window_bytes() const noexcept {
        return windows_.empty() ? 0 : windows_[0].bytes;
    }
    bool verify_boundaries() const noexcept;
    const GuestAddressSpace& addresses() const noexcept { return addresses_; }

private:
    std::vector<Window> windows_;
    GuestAddressSpace addresses_;
    uint32_t gpu_va_bits_ = 0;
    uint32_t resource_bits_ = 0;
};
} // namespace scarlett::gpu
