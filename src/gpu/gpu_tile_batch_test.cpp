#include "device_context.h"
#include "sparse_gpu_windows.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace scarlett::gpu {

// Native host-only test: one UpdateTileMappings per reserved resource maps
// four consecutive logical GPU tiles to four NONconsecutive physical tiles
// inside one 1MiB D3D12 buffer heap. No Xbox ABI or title shader is called.
HRESULT DeviceContext::tile_batch_mapping_test() {
    constexpr uint32_t kTiles = 4;
    constexpr uint64_t kPage = SparseGpuWindows::tile_bytes;
    constexpr uint64_t kBytes = kPage * kTiles;
    constexpr uint64_t kWindow = 16ull * 1024 * 1024 * 1024;
    constexpr uint64_t kFirst = 4ull * 1024 * 1024 * 1024 + kPage * 5;
    constexpr uint64_t kSecond = 12ull * 1024 * 1024 * 1024 + kPage * 11;
    constexpr uint32_t kPhysicalHeapTiles = 16;
    constexpr uint64_t kHeapBytes = uint64_t{kPhysicalHeapTiles} * kPage;

    if (!ready()) {
        last_error_ = initialize();
        if (FAILED(last_error_)) return last_error_;
    }
    Microsoft::WRL::ComPtr<ID3D12Device11> device11;
    last_error_ = acquire_device11(device11.GetAddressOf());
    if (FAILED(last_error_)) return last_error_;

    SparseGpuWindows windows;
    last_error_ = windows.initialize(device11.Get(), kWindow, 2);
    if (FAILED(last_error_)) return last_error_;

    D3D12_HEAP_DESC heap_desc{};
    heap_desc.SizeInBytes = kHeapBytes;
    heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    Microsoft::WRL::ComPtr<ID3D12Heap> heap;
    last_error_ = device11->CreateHeap(
        &heap_desc, IID_PPV_ARGS(heap.GetAddressOf()));
    if (FAILED(last_error_)) return last_error_;

    // Reject malformed requests before any native GPU mapping call.
    const std::array<uint32_t, kTiles> physical_a{6, 2, 9, 0};
    const std::array<uint32_t, kTiles> physical_b{0, 9, 2, 6};
    const std::array<uint32_t, kTiles> repeated{6, 2, 2, 0};
    const std::array<uint32_t, kTiles> out_of_heap{6, 2, 16, 0};
    const std::span<const uint32_t> empty{};
    if (windows.map_tile_selection(nullptr, 0, kFirst, heap.Get(), physical_a) !=
            E_POINTER ||
        windows.map_tile_selection(queue_.Get(), 0, kFirst, heap.Get(), empty) !=
            E_INVALIDARG ||
        windows.map_tile_selection(queue_.Get(), 0, kFirst + 1,
                                   heap.Get(), physical_a) != E_INVALIDARG ||
        windows.map_tile_selection(queue_.Get(), 2, kFirst,
                                   heap.Get(), physical_a) != E_INVALIDARG ||
        windows.map_tile_selection(queue_.Get(), 0, kFirst,
                                   heap.Get(), repeated) != E_INVALIDARG ||
        windows.map_tile_selection(queue_.Get(), 0, kFirst,
                                   heap.Get(), out_of_heap) != E_INVALIDARG ||
        // One native resource only: crossing 8GiB segment boundary fails.
        windows.map_tile_selection(
            queue_.Get(), 0, 8ull * 1024 * 1024 * 1024 - kPage,
            heap.Get(), physical_a) != E_INVALIDARG ||
        windows.mapped_tile_count() != 0 ||
        windows.native_map_update_calls() != 0)
        return last_error_ = E_FAIL;

    Microsoft::WRL::ComPtr<ID3D12Resource> upload, readback;
    last_error_ = create_buffer(
        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
        kBytes, upload);
    if (FAILED(last_error_)) return last_error_;
    last_error_ = create_buffer(
        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
        kBytes, readback);
    if (FAILED(last_error_)) return last_error_;

    const auto fence_queue = [this]() -> HRESULT {
        const uint64_t value = next_fence_++;
        HRESULT hr = queue_->Signal(fence_.Get(), value);
        if (FAILED(hr)) return hr;
        return wait_for_gpu(value);
    };

    std::vector<uint8_t> expected(static_cast<size_t>(kBytes));
    for (uint32_t pass = 0; pass < 2; ++pass) {
        const auto& chosen = pass == 0 ? physical_a : physical_b;
        const uint64_t previous = windows.native_map_update_calls();

        last_error_ = windows.map_tile_selection(
            queue_.Get(), 0, kFirst, heap.Get(), chosen);
        if (FAILED(last_error_)) return last_error_;
        last_error_ = windows.map_tile_selection(
            queue_.Get(), 1, kSecond, heap.Get(), chosen);
        if (FAILED(last_error_)) return last_error_;
        if (windows.native_map_update_calls() != previous + 2 ||
            windows.mapped_tile_count() != kTiles * 2 ||
            windows.map_tile_selection(
                queue_.Get(), 0, kFirst, heap.Get(), chosen) !=
                    HRESULT_FROM_WIN32(ERROR_BUSY) ||
            windows.native_map_update_calls() != previous + 2)
            return last_error_ = E_FAIL;

        // Ensure both real GPU resource tile mappings have completed.
        last_error_ = fence_queue();
        if (FAILED(last_error_)) return last_error_;

        for (size_t i = 0; i < expected.size(); ++i) {
            const size_t page = i / kPage;
            const size_t pos = i % kPage;
            expected[i] = static_cast<uint8_t>(
                (pos * 37 + (pos >> 3) * 11 + chosen[page] * 23 +
                 page * 19 + pass * 71) & 255);
        }
        void* mapped = nullptr;
        const D3D12_RANGE no_read{0, 0};
        last_error_ = upload->Map(0, &no_read, &mapped);
        if (FAILED(last_error_) || !mapped)
            return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
        std::memcpy(mapped, expected.data(), expected.size());
        const D3D12_RANGE written{0, expected.size()};
        upload->Unmap(0, &written);

        uint64_t native_first = 0, native_second = 0;
        ID3D12Resource* first = windows.tile_resource(
            0, kFirst, &native_first);
        ID3D12Resource* second = windows.tile_resource(
            1, kSecond, &native_second);
        if (!first || !second || first == second ||
            native_first != 5 * kPage || native_second != 11 * kPage)
            return last_error_ = E_FAIL;

        last_error_ = allocator_->Reset();
        if (FAILED(last_error_)) return last_error_;
        last_error_ = command_list_->Reset(allocator_.Get(), nullptr);
        if (FAILED(last_error_)) return last_error_;
        D3D12_RESOURCE_BARRIER state{};
        state.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        state.Transition.pResource = first;
        state.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        state.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        state.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        command_list_->ResourceBarrier(1, &state);
        command_list_->CopyBufferRegion(
            first, native_first, upload.Get(), 0, kBytes);
        state.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        state.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        command_list_->ResourceBarrier(1, &state);

        D3D12_RESOURCE_BARRIER alias{};
        alias.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        alias.Aliasing.pResourceBefore = first;
        alias.Aliasing.pResourceAfter = second;
        command_list_->ResourceBarrier(1, &alias);
        state.Transition.pResource = second;
        state.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        state.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        command_list_->ResourceBarrier(1, &state);
        command_list_->CopyBufferRegion(
            readback.Get(), 0, second, native_second, kBytes);
        state.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        state.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        command_list_->ResourceBarrier(1, &state);

        last_error_ = command_list_->Close();
        if (FAILED(last_error_)) return last_error_;
        ID3D12CommandList* lists[]{command_list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        ++submitted_;
        last_error_ = fence_queue();
        if (FAILED(last_error_)) return last_error_;

        void* gpu_bytes = nullptr;
        const D3D12_RANGE readable{0, expected.size()};
        last_error_ = readback->Map(0, &readable, &gpu_bytes);
        if (FAILED(last_error_) || !gpu_bytes)
            return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
        const bool matched = std::memcmp(
            gpu_bytes, expected.data(), expected.size()) == 0;
        readback->Unmap(0, &no_read);
        if (!matched) return last_error_ = HRESULT_FROM_WIN32(ERROR_CRC);

        last_error_ = windows.unmap_tiles(
            queue_.Get(), 1, kSecond, kTiles);
        if (FAILED(last_error_)) return last_error_;
        last_error_ = windows.unmap_tiles(
            queue_.Get(), 0, kFirst, kTiles);
        if (FAILED(last_error_)) return last_error_;
        if (windows.mapped_tile_count() != 0 ||
            windows.native_unmap_update_calls() != uint64_t{2} * (pass + 1))
            return last_error_ = E_FAIL;
        // Never recycle the physical tiles until NULL unmaps are acknowledged.
        last_error_ = fence_queue();
        if (FAILED(last_error_)) return last_error_;
    }

    tile_mapping_updates_ = windows.native_map_update_calls();
    tile_unmapping_updates_ = windows.native_unmap_update_calls();
    if (tile_mapping_updates_ != 4 || tile_unmapping_updates_ != 4 ||
        submitted_ != 2 || windows.mapped_tile_count() != 0)
        return last_error_ = E_FAIL;
    windows.clear();
    return last_error_ = S_OK;
}
} // namespace scarlett::gpu
