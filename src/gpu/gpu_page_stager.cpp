#include "gpu_page_stager.h"
#include <cstring>

namespace scarlett::gpu {
GpuPageStager::~GpuPageStager() { close(); }

HRESULT GpuPageStager::create_buffer(
    D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
    Microsoft::WRL::ComPtr<ID3D12Resource>& out) noexcept {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = page_bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return device_->CreateCommittedResource(
        &props, D3D12_HEAP_FLAG_NONE, &desc, state,
        nullptr, IID_PPV_ARGS(out.GetAddressOf()));
}

HRESULT GpuPageStager::sync() noexcept {
    if (!queue_ || !fence_ || !event_) return E_UNEXPECTED;
    if (next_fence_ == UINT64_MAX) return E_FAIL;
    const uint64_t value = next_fence_++;
    HRESULT hr = queue_->Signal(fence_.Get(), value);
    if (FAILED(hr)) return hr;
    if (fence_->GetCompletedValue() < value) {
        hr = fence_->SetEventOnCompletion(value, event_);
        if (FAILED(hr)) return hr;
        const DWORD outcome = WaitForSingleObject(event_, 10000);
        if (outcome != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(
                outcome == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    }
    if (fence_->GetCompletedValue() < value) return E_FAIL;
    last_completed_ = value;
    return S_OK;
}

HRESULT GpuPageStager::open(
    ID3D12Device11* device, SparseGpuWindows& windows,
    size_t source_window, uint64_t source_offset,
    size_t alias_window, uint64_t alias_offset) noexcept {
    if (opened_ || windows_ || device_)
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    if (!device) return E_POINTER;
    if (source_window == alias_window && source_offset == alias_offset)
        return E_INVALIDARG;
    if (source_offset % page_bytes || alias_offset % page_bytes)
        return E_INVALIDARG;

    uint64_t source_local = 0, alias_local = 0;
    ID3D12Resource* src = windows.tile_resource(
        source_window, source_offset, &source_local);
    ID3D12Resource* dst = windows.tile_resource(
        alias_window, alias_offset, &alias_local);
    if (!src || !dst || src == dst ||
        source_local > src->GetDesc().Width ||
        alias_local > dst->GetDesc().Width ||
        page_bytes > src->GetDesc().Width - source_local ||
        page_bytes > dst->GetDesc().Width - alias_local)
        return E_INVALIDARG;

    D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
    HRESULT hr = device->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts));
    if (FAILED(hr)) return hr;
    if (opts.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_2)
        return DXGI_ERROR_UNSUPPORTED;
    // A closed stager can begin a new independent GPU session.
    next_fence_ = 1;
    last_completed_ = 0;
    submissions_ = 0;
    alias_active_ = false;
    device_ = device;
    windows_ = &windows;
    source_window_ = source_window;
    alias_window_ = alias_window;
    source_offset_ = source_offset;
    alias_offset_ = alias_offset;
    source_local_ = source_local;
    alias_local_ = alias_local;
    auto fail = [this](HRESULT error) {
        close(); // best-effort cleanup, including partial GPU tile mappings
        return error;
    };

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device_->CreateCommandQueue(
        &queue_desc, IID_PPV_ARGS(queue_.GetAddressOf()));
    if (FAILED(hr)) return fail(hr);
    hr = device_->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator_.GetAddressOf()));
    if (FAILED(hr)) return fail(hr);
    hr = device_->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
        IID_PPV_ARGS(commands_.GetAddressOf()));
    if (FAILED(hr)) return fail(hr);
    hr = commands_->Close();
    if (FAILED(hr)) return fail(hr);
    hr = device_->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.GetAddressOf()));
    if (FAILED(hr)) return fail(hr);
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) return fail(HRESULT_FROM_WIN32(GetLastError()));

    D3D12_HEAP_DESC tile_heap{};
    tile_heap.SizeInBytes = page_bytes;
    tile_heap.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    tile_heap.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    tile_heap.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    hr = device_->CreateHeap(
        &tile_heap, IID_PPV_ARGS(physical_tile_.GetAddressOf()));
    if (FAILED(hr)) return fail(hr);
    hr = create_buffer(D3D12_HEAP_TYPE_UPLOAD,
                       D3D12_RESOURCE_STATE_GENERIC_READ, upload_);
    if (FAILED(hr)) return fail(hr);
    hr = create_buffer(D3D12_HEAP_TYPE_READBACK,
                       D3D12_RESOURCE_STATE_COPY_DEST, readback_);
    if (FAILED(hr)) return fail(hr);

    hr = windows_->map_tiles(
        queue_.Get(), source_window_, source_offset_, physical_tile_.Get(), 0, 1);
    if (FAILED(hr)) return fail(hr);
    first_mapped_ = true;
    hr = windows_->map_tiles(
        queue_.Get(), alias_window_, alias_offset_, physical_tile_.Get(), 0, 1);
    if (FAILED(hr)) return fail(hr);
    second_mapped_ = true;
    hr = sync(); // both tile mappings complete before any copy starts
    if (FAILED(hr)) return fail(hr);
    opened_ = true;
    return S_OK;
}

HRESULT GpuPageStager::roundtrip(
    std::span<const uint8_t> input, std::span<uint8_t> output) noexcept {
    if (!opened_ || !windows_ || !commands_ || !upload_ || !readback_)
        return E_UNEXPECTED;
    if (input.size() != page_bytes || output.size() != page_bytes)
        return E_INVALIDARG;
    const uintptr_t begin_in = reinterpret_cast<uintptr_t>(input.data());
    const uintptr_t begin_out = reinterpret_cast<uintptr_t>(output.data());
    if (begin_in <= begin_out ? begin_out - begin_in < page_bytes
                              : begin_in - begin_out < page_bytes)
        return E_INVALIDARG;

    void* upload_pointer = nullptr;
    const D3D12_RANGE empty_range{0, 0};
    HRESULT hr = upload_->Map(0, &empty_range, &upload_pointer);
    if (FAILED(hr)) return hr;
    if (!upload_pointer) return E_FAIL;
    std::memcpy(upload_pointer, input.data(), page_bytes);
    const D3D12_RANGE written_range{0, page_bytes};
    upload_->Unmap(0, &written_range);

    hr = allocator_->Reset();
    if (FAILED(hr)) return hr;
    hr = commands_->Reset(allocator_.Get(), nullptr);
    if (FAILED(hr)) return hr;
    uint64_t ignored_offset = 0;
    ID3D12Resource* src = windows_->tile_resource(
        source_window_, source_offset_, &ignored_offset);
    ID3D12Resource* dst = windows_->tile_resource(
        alias_window_, alias_offset_, &ignored_offset);
    if (!src || !dst) return E_FAIL;

    if (alias_active_) {
        D3D12_RESOURCE_BARRIER switching{};
        switching.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        switching.Aliasing.pResourceBefore = dst;
        switching.Aliasing.pResourceAfter = src;
        commands_->ResourceBarrier(1, &switching);
    }
    D3D12_RESOURCE_BARRIER state{};
    state.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    state.Transition.pResource = src;
    state.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    state.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    state.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    commands_->ResourceBarrier(1, &state);
    commands_->CopyBufferRegion(src, source_local_, upload_.Get(), 0, page_bytes);
    state.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    state.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    commands_->ResourceBarrier(1, &state);

    D3D12_RESOURCE_BARRIER alias{};
    alias.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    alias.Aliasing.pResourceBefore = src;
    alias.Aliasing.pResourceAfter = dst;
    commands_->ResourceBarrier(1, &alias);
    state.Transition.pResource = dst;
    state.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    state.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commands_->ResourceBarrier(1, &state);
    commands_->CopyBufferRegion(readback_.Get(), 0, dst, alias_local_, page_bytes);
    state.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    state.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    commands_->ResourceBarrier(1, &state);

    hr = commands_->Close();
    if (FAILED(hr)) return hr;
    ID3D12CommandList* lists[]{commands_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    ++submissions_;
    alias_active_ = true;
    hr = sync();
    if (FAILED(hr)) {
        const HRESULT removed = device_->GetDeviceRemovedReason();
        return FAILED(removed) ? removed : hr;
    }
    void* result = nullptr;
    const D3D12_RANGE read_range{0, page_bytes};
    hr = readback_->Map(0, &read_range, &result);
    if (FAILED(hr)) return hr;
    if (!result) return E_FAIL;
    std::memcpy(output.data(), result, page_bytes);
    readback_->Unmap(0, &empty_range);
    return S_OK;
}

HRESULT GpuPageStager::close() noexcept {
    HRESULT result = S_OK;
    if (queue_ && fence_ && event_) {
        const HRESULT hr = sync();
        if (FAILED(hr)) result = hr;
    }
    if (windows_ && queue_) {
        if (second_mapped_) {
            const HRESULT hr = windows_->unmap_tiles(
                queue_.Get(), alias_window_, alias_offset_, 1);
            if (FAILED(hr) && SUCCEEDED(result)) result = hr;
        }
        if (first_mapped_) {
            const HRESULT hr = windows_->unmap_tiles(
                queue_.Get(), source_window_, source_offset_, 1);
            if (FAILED(hr) && SUCCEEDED(result)) result = hr;
        }
        if ((first_mapped_ || second_mapped_) && fence_ && event_) {
            const HRESULT hr = sync();
            if (FAILED(hr) && SUCCEEDED(result)) result = hr;
        }
    }
    first_mapped_ = false;
    second_mapped_ = false;
    opened_ = false;
    alias_active_ = false;
    windows_ = nullptr;
    upload_.Reset();
    readback_.Reset();
    physical_tile_.Reset();
    commands_.Reset();
    allocator_.Reset();
    queue_.Reset();
    device_.Reset();
    fence_.Reset();
    if (event_) { CloseHandle(event_); event_ = nullptr; }
    return result;
}
} // namespace scarlett::gpu
