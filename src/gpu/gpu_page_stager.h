#pragma once
#include "sparse_gpu_windows.h"
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <span>

namespace scarlett::gpu {
// Reusable independent host 64KiB CPU<->GPU transfer; not Xbox unified memory.
// Owns or retains a DIRECT queue, native command list, fence, one real GPU heap tile,
// upload/readback buffers. Caller keeps SparseGpuWindows alive through close().
class GpuPageStager final {
public:
    static constexpr size_t page_bytes = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    ~GpuPageStager();
    GpuPageStager() = default;
    GpuPageStager(const GpuPageStager&) = delete;
    GpuPageStager& operator=(const GpuPageStager&) = delete;

    HRESULT open(ID3D12Device11* device, SparseGpuWindows& windows,
                 size_t source_window, uint64_t source_offset,
                 size_t alias_window, uint64_t alias_offset,
                 ID3D12CommandQueue* shared_queue = nullptr,
                 ID3D12Heap* shared_tile_heap = nullptr,
                 uint32_t shared_heap_tile = 0,
                 bool defer_tile_mapping = false) noexcept;
    // Both spans must be exactly one page and nonoverlapping. Copies GPU data
    // into output only AFTER a completed fence; does not update CPU XMem.
    HRESULT roundtrip(std::span<const uint8_t> cpu_input,
                      std::span<uint8_t> cpu_output) noexcept;
    // Only release native resources after all queued work AND both NULL tile
    // mappings are fenced. Failure preserves the session for a retry.
    HRESULT close() noexcept;
    bool is_open() const noexcept { return opened_; }
    uint64_t submissions() const noexcept { return submissions_; }
    uint64_t completed_fence() const noexcept { return last_completed_; }
private:
    friend class GuestGpuPageRegistry;
    // Batch-only phases: registry owns queue submission and one shared fence.
    HRESULT prepare_roundtrip(std::span<const uint8_t> input,
                              std::span<uint8_t> output) noexcept;
    HRESULT collect_readback(std::span<uint8_t> output) noexcept;
    void cancel_prepared() noexcept { prepared_ = false; }
    void mark_submitted() noexcept {
        prepared_ = false;
        in_flight_ = true;
        alias_active_ = true;
        ++submissions_;
    }
    HRESULT sync() noexcept;
    HRESULT create_buffer(D3D12_HEAP_TYPE, D3D12_RESOURCE_STATES,
                          Microsoft::WRL::ComPtr<ID3D12Resource>&) noexcept;
    bool opened_ = false, first_mapped_ = false, second_mapped_ = false;
    bool alias_active_ = false;
    bool prepared_ = false;
    bool in_flight_ = false;
    // Deterministic negative test: fail a chosen future fence wait without
    // blocking or mutating the real underlying GPU queue. -1 means disabled.
    int test_sync_failure_countdown_ = -1;
    SparseGpuWindows* windows_ = nullptr;
    size_t source_window_ = 0, alias_window_ = 0;
    uint64_t source_offset_ = 0, alias_offset_ = 0;
    uint64_t source_local_ = 0, alias_local_ = 0;
    uint64_t next_fence_ = 1, last_completed_ = 0, submissions_ = 0;
    Microsoft::WRL::ComPtr<ID3D12Device11> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D12Heap> physical_tile_;
    uint32_t physical_heap_tile_ = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_, readback_;
    HANDLE event_ = nullptr;
};
} // namespace scarlett::gpu
