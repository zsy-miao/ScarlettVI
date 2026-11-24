#pragma once
#include "guest_address_space.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <map>
#include <vector>

namespace scarlett::gpu {

// Independently authored host GPU virtual-address backend. A logical Xbox
// address window is split into ≤4GiB D3D12 reserved buffer segments because
// this machine's D3D12 driver does not round-trip mapped tiles at X >= 65536.
// The guest range stays continuous in GuestAddressSpace; native GPU VAs do NOT.
class SparseGpuWindows final {
public:
    static constexpr uint64_t segment_bytes = 4ull * 1024 * 1024 * 1024;
    static constexpr uint64_t tile_bytes = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    struct Segment {
        uint64_t logical_offset = 0;
        uint64_t bytes = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> reserved;
    };
    struct Window {
        uint64_t synthetic_guest_base = 0;
        uint64_t bytes = 0;
        std::vector<Segment> segments;
        struct TileMapping {
            Microsoft::WRL::ComPtr<ID3D12Heap> heap;
            uint32_t heap_tile = 0;
        };
        std::map<uint64_t, TileMapping> mapped_tiles;
    };
    SparseGpuWindows() = default;
    SparseGpuWindows(const SparseGpuWindows&) = delete;
    SparseGpuWindows& operator=(const SparseGpuWindows&) = delete;
    ~SparseGpuWindows();

    HRESULT initialize(ID3D12Device11* device, uint64_t window_bytes,
                       uint32_t window_count) noexcept;
    // All queue submissions, including tile unmaps, must be fenced first.
    void clear() noexcept;
    size_t window_count() const noexcept { return windows_.size(); }
    size_t segment_count() const noexcept;
    size_t mapped_tile_count() const noexcept;
    uint32_t gpu_va_bits_per_process() const noexcept { return gpu_va_bits_; }
    uint32_t gpu_va_bits_per_resource() const noexcept { return resource_bits_; }
    uint64_t window_bytes() const noexcept {
        return windows_.empty() ? 0 : windows_[0].bytes;
    }
    bool verify_boundaries() const noexcept;

    // Enqueues real host UpdateTileMappings. Supports one contiguous range
    // within one ≤4GiB segment. Cross-segment calls must be split by caller.
    // A success HRESULT means validation + enqueue; a GPU readback proves work.
    HRESULT map_tiles(ID3D12CommandQueue* queue, size_t window_index,
                      uint64_t byte_offset, ID3D12Heap* heap,
                      uint32_t first_heap_tile, uint32_t tile_count) noexcept;
    HRESULT unmap_tiles(ID3D12CommandQueue* queue, size_t window_index,
                        uint64_t byte_offset, uint32_t tile_count) noexcept;

    // Translate logical window offsets into a native resource and local
    // byte offset. The resource belongs to SparseGpuWindows (no AddRef).
    ID3D12Resource* tile_resource(size_t window_index, uint64_t byte_offset,
                                   uint64_t* local_offset) const noexcept;

    const GuestAddressSpace& addresses() const noexcept { return addresses_; }
private:
    std::vector<Window> windows_;
    // Heap refs retired during async null mappings are held until clear(),
    // which MUST only occur after queue fence completion.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Heap>> retired_heaps_;
    GuestAddressSpace addresses_;
    uint32_t gpu_va_bits_ = 0;
    uint32_t resource_bits_ = 0;
};
} // namespace scarlett::gpu
