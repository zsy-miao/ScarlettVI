#include "sparse_gpu_windows.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace scarlett::gpu {
SparseGpuWindows::~SparseGpuWindows() { clear(); }

void SparseGpuWindows::clear() noexcept {
    // A caller must fence queued GPU work before destroying tile heap refs.
    for (const auto& window : windows_)
        for (const auto& segment : window.segments)
            addresses_.unregister_buffer(window.synthetic_guest_base +
                                         segment.logical_offset);
    windows_.clear();
    retired_heaps_.clear();
    gpu_va_bits_ = 0;
    resource_bits_ = 0;
}

HRESULT SparseGpuWindows::initialize(ID3D12Device11* device,
                                     uint64_t bytes, uint32_t count) noexcept {
    if (!device) return E_POINTER;
    if (!count || count > 4 || !bytes ||
        bytes % GuestAddressSpace::guest_alignment != 0 ||
        bytes > 32ull * 1024 * 1024 * 1024)
        return E_INVALIDARG;
    if (!windows_.empty()) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);

    D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT support{};
    HRESULT hr = device->CheckFeatureSupport(
        D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &support, sizeof(support));
    if (FAILED(hr)) return hr;
    gpu_va_bits_ = support.MaxGPUVirtualAddressBitsPerProcess;
    resource_bits_ = support.MaxGPUVirtualAddressBitsPerResource;
    if (gpu_va_bits_ >= 63 || resource_bits_ >= 63 ||
        gpu_va_bits_ < 36 || resource_bits_ < 36 ||
        (uint64_t{1} << resource_bits_) < std::min(bytes, segment_bytes) ||
        (uint64_t{1} << gpu_va_bits_) < bytes * (count + 1ull))
        return DXGI_ERROR_UNSUPPORTED;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    constexpr uint64_t guest_start = 0x400000000000ull;
    constexpr uint64_t guest_stride = 64ull * 1024 * 1024 * 1024;
    try {
        windows_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Window window{};
            window.synthetic_guest_base = guest_start + i * guest_stride;
            window.bytes = bytes;
            windows_.push_back(std::move(window));
            auto& active = windows_.back();
            const size_t segment_count =
                static_cast<size_t>((bytes + segment_bytes - 1) / segment_bytes);
            active.segments.reserve(segment_count);
            for (uint64_t offset = 0; offset < bytes; offset += segment_bytes) {
                Segment segment{};
                segment.logical_offset = offset;
                segment.bytes = std::min(segment_bytes, bytes - offset);
                desc.Width = segment.bytes;
                hr = device->CreateReservedResource(
                    &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                    IID_PPV_ARGS(segment.reserved.GetAddressOf()));
                if (FAILED(hr)) {
                    clear();
                    return hr;
                }
                // Add the resource to the lifetime registry BEFORE trying
                // to register the guest address, so clear() always sees it.
                active.segments.push_back(std::move(segment));
                const auto& created = active.segments.back();
                if (!addresses_.register_buffer(
                        active.synthetic_guest_base + created.logical_offset,
                        created.bytes, created.reserved.Get())) {
                    clear();
                    return E_FAIL;
                }
            }
        }
    } catch (...) {
        clear();
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

size_t SparseGpuWindows::segment_count() const noexcept {
    size_t count = 0;
    for (const auto& window : windows_) count += window.segments.size();
    return count;
}

size_t SparseGpuWindows::mapped_tile_count() const noexcept {
    size_t count = 0;
    for (const auto& window : windows_) count += window.mapped_tiles.size();
    return count;
}

ID3D12Resource* SparseGpuWindows::tile_resource(size_t index,
                                                uint64_t byte_offset,
                                                uint64_t* local_offset) const noexcept {
    if (!local_offset || index >= windows_.size() ||
        byte_offset >= windows_[index].bytes)
        return nullptr;
    const auto& window = windows_[index];
    const size_t segment_index = static_cast<size_t>(byte_offset / segment_bytes);
    if (segment_index >= window.segments.size()) return nullptr;
    const auto& segment = window.segments[segment_index];
    *local_offset = byte_offset - segment.logical_offset;
    return segment.reserved.Get();
}

HRESULT SparseGpuWindows::map_tiles(ID3D12CommandQueue* queue,
                                    size_t index, uint64_t byte_offset,
                                    ID3D12Heap* heap, uint32_t heap_start,
                                    uint32_t tile_count) noexcept {
    if (!queue || !heap) return E_POINTER;
    if (index >= windows_.size() || !tile_count || tile_count > 4096 ||
        byte_offset % tile_bytes) return E_INVALIDARG;
    const auto& window = windows_[index];
    uint64_t local_offset = 0;
    ID3D12Resource* resource = tile_resource(index, byte_offset, &local_offset);
    if (!resource || local_offset % tile_bytes ||
        uint64_t{tile_count} * tile_bytes > window.bytes - byte_offset)
        return E_INVALIDARG;

    const size_t segment_index = static_cast<size_t>(byte_offset / segment_bytes);
    const auto& segment = window.segments[segment_index];
    if (uint64_t{tile_count} * tile_bytes > segment.bytes - local_offset ||
        heap_start > UINT32_MAX - tile_count ||
        uint64_t{heap_start + tile_count} * tile_bytes > heap->GetDesc().SizeInBytes)
        return E_INVALIDARG;

    auto& mappings = windows_[index].mapped_tiles;
    const uint64_t global_tile = byte_offset / tile_bytes;
    for (uint32_t i = 0; i < tile_count; ++i)
        if (mappings.contains(global_tile + i))
            return HRESULT_FROM_WIN32(ERROR_BUSY);

    D3D12_TILED_RESOURCE_COORDINATE coord{};
    coord.X = static_cast<UINT>(local_offset / tile_bytes);
    D3D12_TILE_REGION_SIZE region{};
    region.NumTiles = tile_count;
    region.UseBox = FALSE;
    constexpr D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NONE;
    const UINT offset = heap_start;
    const UINT count = tile_count;

    // Retain backing heaps before enqueueing an update that returns void.
    try {
        for (uint32_t i = 0; i < tile_count; ++i) {
            Window::TileMapping mapping;
            mapping.heap = heap;
            mapping.heap_tile = heap_start + i;
            mappings.emplace(global_tile + i, std::move(mapping));
        }
    } catch (...) {
        for (uint32_t i = 0; i < tile_count; ++i)
            mappings.erase(global_tile + i);
        return E_OUTOFMEMORY;
    }
    queue->UpdateTileMappings(resource, 1, &coord, &region, heap, 1,
                              &flags, &offset, &count,
                              D3D12_TILE_MAPPING_FLAG_NONE);
    return S_OK;
}

HRESULT SparseGpuWindows::unmap_tiles(ID3D12CommandQueue* queue,
                                      size_t index, uint64_t byte_offset,
                                      uint32_t tile_count) noexcept {
    if (!queue) return E_POINTER;
    if (index >= windows_.size() || !tile_count || tile_count > 4096 ||
        byte_offset % tile_bytes) return E_INVALIDARG;
    auto& window = windows_[index];
    uint64_t local_offset = 0;
    ID3D12Resource* resource = tile_resource(index, byte_offset, &local_offset);
    if (!resource || uint64_t{tile_count} * tile_bytes >
                         window.bytes - byte_offset)
        return E_INVALIDARG;
    const auto& segment = window.segments[
        static_cast<size_t>(byte_offset / segment_bytes)];
    if (uint64_t{tile_count} * tile_bytes > segment.bytes - local_offset)
        return E_INVALIDARG;

    const uint64_t global_tile = byte_offset / tile_bytes;
    for (uint32_t i = 0; i < tile_count; ++i)
        if (!window.mapped_tiles.contains(global_tile + i))
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

    D3D12_TILED_RESOURCE_COORDINATE coord{};
    coord.X = static_cast<UINT>(local_offset / tile_bytes);
    D3D12_TILE_REGION_SIZE region{};
    region.NumTiles = tile_count;
    region.UseBox = FALSE;
    constexpr D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NULL;
    const UINT count = tile_count;
    try {
        retired_heaps_.reserve(retired_heaps_.size() + tile_count);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    // Retain the heap until the caller fences and invokes clear().
    for (uint32_t i = 0; i < tile_count; ++i)
        retired_heaps_.push_back(
            window.mapped_tiles.at(global_tile + i).heap);
    queue->UpdateTileMappings(resource, 1, &coord, &region, nullptr, 1,
                              &flags, nullptr, &count,
                              D3D12_TILE_MAPPING_FLAG_NONE);
    for (uint32_t i = 0; i < tile_count; ++i)
        window.mapped_tiles.erase(global_tile + i);
    return S_OK;
}

bool SparseGpuWindows::verify_boundaries() const noexcept {
    if (windows_.empty() || addresses_.resource_count() != segment_count())
        return false;
    for (const auto& window : windows_) {
        uint64_t covered = 0;
        for (const auto& segment : window.segments) {
            if (segment.logical_offset != covered ||
                segment.bytes == 0 || segment.bytes > segment_bytes)
                return false;
            const uint64_t address =
                window.synthetic_guest_base + segment.logical_offset;
            const auto first = addresses_.resolve(
                address, GuestAddressSpace::guest_alignment);
            const auto last = addresses_.resolve(
                address + segment.bytes - GuestAddressSpace::guest_alignment,
                GuestAddressSpace::guest_alignment);
            const auto cross_boundary = addresses_.resolve(
                address + segment.bytes - 4, 8);
            if (!first || !last || cross_boundary ||
                first->resource.Get() != segment.reserved.Get() ||
                last->resource.Get() != segment.reserved.Get() ||
                last->host_gpu_va != first->host_gpu_va + segment.bytes -
                                     GuestAddressSpace::guest_alignment)
                return false;
            covered += segment.bytes;
        }
        if (covered != window.bytes) return false;
    }
    return true;
}
} // namespace scarlett::gpu
