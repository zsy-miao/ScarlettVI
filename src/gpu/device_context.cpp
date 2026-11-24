#include "device_context.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace scarlett::gpu {
namespace {
constexpr UINT kImageWidth = 128;
constexpr UINT kImageHeight = 64;
constexpr UINT kPixelBytes = 4;

// Deterministic generated color fixture, not a game asset.
std::array<uint8_t, 4> pixel_value(UINT x, UINT y) {
    return {
        static_cast<uint8_t>((x * 3 + y * 7) & 0xff),
        static_cast<uint8_t>((x ^ (y * 11)) & 0xff),
        static_cast<uint8_t>((x * 13 + y * 5) & 0xff),
        static_cast<uint8_t>(255 - ((x + y) & 0x7f))
    };
}
D3D12_RESOURCE_DESC buffer_description(uint64_t bytes) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}
} // namespace

DeviceContext::DeviceContext() = default;
DeviceContext::~DeviceContext() {
    if (fence_event_) {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
}
HRESULT DeviceContext::initialize() {
    if (ready()) return S_OK;
    last_error_ = CreateDXGIFactory2(0, IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf()));
    if (FAILED(last_error_)) return last_error_;

    for (UINT i = 0; i < 32; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
        const HRESULT hr = factory_->EnumAdapters1(i, candidate.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) { last_error_ = hr; return hr; }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(candidate->GetDesc1(&desc)) ||
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        Microsoft::WRL::ComPtr<ID3D12Device> maybe_device;
        const HRESULT device_hr = D3D12CreateDevice(
            candidate.Get(), D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(maybe_device.GetAddressOf()));
        if (FAILED(device_hr)) continue;
        adapter_ = candidate;
        device_ = maybe_device;
        adapter_name_ = desc.Description;
        break;
    }
    if (!device_) {
        last_error_ = DXGI_ERROR_UNSUPPORTED;
        return last_error_;
    }
    D3D12_COMMAND_QUEUE_DESC queue_info{};
    queue_info.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    last_error_ = device_->CreateCommandQueue(
        &queue_info, IID_PPV_ARGS(queue_.ReleaseAndGetAddressOf()));
    if (FAILED(last_error_)) return last_error_;
    last_error_ = device_->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(allocator_.ReleaseAndGetAddressOf()));
    if (FAILED(last_error_)) return last_error_;
    last_error_ = device_->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
        IID_PPV_ARGS(command_list_.ReleaseAndGetAddressOf()));
    if (FAILED(last_error_)) return last_error_;
    last_error_ = command_list_->Close();
    if (FAILED(last_error_)) return last_error_;
    last_error_ = device_->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.ReleaseAndGetAddressOf()));
    if (FAILED(last_error_)) return last_error_;
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
        last_error_ = HRESULT_FROM_WIN32(GetLastError());
        return last_error_;
    }
    last_error_ = S_OK;
    return S_OK;
}

HRESULT DeviceContext::create_buffer(D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                    uint64_t bytes,
                                    Microsoft::WRL::ComPtr<ID3D12Resource>& output) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    const auto description = buffer_description(bytes);
    return device_->CreateCommittedResource(
        &properties, D3D12_HEAP_FLAG_NONE, &description, state,
        nullptr, IID_PPV_ARGS(output.ReleaseAndGetAddressOf()));
}

HRESULT DeviceContext::wait_for_gpu(uint64_t value) {
    if (fence_->GetCompletedValue() >= value)
        return S_OK;
    HRESULT hr = fence_->SetEventOnCompletion(value, fence_event_);
    if (FAILED(hr)) return hr;
    const DWORD wait = WaitForSingleObject(fence_event_, 10000);
    if (wait != WAIT_OBJECT_0)
        return wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT)
                                    : HRESULT_FROM_WIN32(GetLastError());
    if (fence_->GetCompletedValue() < value)
        return E_FAIL;
    return S_OK;
}

HRESULT DeviceContext::texture_roundtrip_test() {
    if (!ready()) {
        last_error_ = initialize();
        if (FAILED(last_error_)) return last_error_;
    }

    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = kImageWidth;
    texture.Height = kImageHeight;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT number_rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total_bytes = 0;
    device_->GetCopyableFootprints(&texture, 0, 1, 0, &layout, &number_rows,
                                   &row_bytes, &total_bytes);
    if (number_rows != kImageHeight || row_bytes != kImageWidth * kPixelBytes ||
        total_bytes == 0 || total_bytes > 32ull * 1024ull * 1024ull ||
        layout.Footprint.RowPitch < row_bytes) {
        last_error_ = E_INVALIDARG;
        return last_error_;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    last_error_ = create_buffer(D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ, total_bytes, upload);
    if (FAILED(last_error_)) return last_error_;
    D3D12_HEAP_PROPERTIES default_properties{};
    default_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    last_error_ = device_->CreateCommittedResource(
        &default_properties, D3D12_HEAP_FLAG_NONE, &texture,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(output.ReleaseAndGetAddressOf()));
    if (FAILED(last_error_)) return last_error_;
    last_error_ = create_buffer(D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_STATE_COPY_DEST, total_bytes, readback);
    if (FAILED(last_error_)) return last_error_;

    uint8_t* upload_data = nullptr;
    const D3D12_RANGE no_cpu_reads{0, 0};
    last_error_ = upload->Map(0, &no_cpu_reads,
                              reinterpret_cast<void**>(&upload_data));
    if (FAILED(last_error_) || !upload_data) {
        last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
        return last_error_;
    }
    std::memset(upload_data, 0, static_cast<size_t>(total_bytes));
    for (UINT y = 0; y < kImageHeight; ++y) {
        auto* dst = upload_data + layout.Offset + static_cast<size_t>(y) * layout.Footprint.RowPitch;
        for (UINT x = 0; x < kImageWidth; ++x) {
            const auto value = pixel_value(x, y);
            std::memcpy(dst + x * kPixelBytes, value.data(), kPixelBytes);
        }
    }
    const D3D12_RANGE written{0, static_cast<SIZE_T>(total_bytes)};
    upload->Unmap(0, &written);

    last_error_ = allocator_->Reset();
    if (FAILED(last_error_)) return last_error_;
    last_error_ = command_list_->Reset(allocator_.Get(), nullptr);
    if (FAILED(last_error_)) return last_error_;

    D3D12_TEXTURE_COPY_LOCATION upload_location{};
    upload_location.pResource = upload.Get();
    upload_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    upload_location.PlacedFootprint = layout;
    D3D12_TEXTURE_COPY_LOCATION texture_location{};
    texture_location.pResource = output.Get();
    texture_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    texture_location.SubresourceIndex = 0;
    command_list_->CopyTextureRegion(
        &texture_location, 0, 0, 0, &upload_location, nullptr);

    D3D12_RESOURCE_BARRIER transition{};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition.pResource = output.Get();
    transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    command_list_->ResourceBarrier(1, &transition);

    D3D12_TEXTURE_COPY_LOCATION readback_location{};
    readback_location.pResource = readback.Get();
    readback_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    readback_location.PlacedFootprint = layout;
    command_list_->CopyTextureRegion(
        &readback_location, 0, 0, 0, &texture_location, nullptr);
    last_error_ = command_list_->Close();
    if (FAILED(last_error_)) return last_error_;

    ID3D12CommandList* const lists[]{command_list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    ++submitted_;
    const uint64_t signal = next_fence_++;
    last_error_ = queue_->Signal(fence_.Get(), signal);
    if (FAILED(last_error_)) return last_error_;
    last_error_ = wait_for_gpu(signal);
    if (FAILED(last_error_)) {
        const HRESULT removed = device_->GetDeviceRemovedReason();
        if (FAILED(removed)) last_error_ = removed;
        return last_error_;
    }

    void* mapped_readback = nullptr;
    const D3D12_RANGE cpu_reads{0, static_cast<SIZE_T>(total_bytes)};
    last_error_ = readback->Map(0, &cpu_reads, &mapped_readback);
    if (FAILED(last_error_) || !mapped_readback) {
        last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
        return last_error_;
    }
    const auto* copied_data = static_cast<const uint8_t*>(mapped_readback);
    bool accurate = true;
    for (UINT y = 0; y < kImageHeight && accurate; ++y) {
        const auto* row = copied_data + layout.Offset +
                          static_cast<size_t>(y) * layout.Footprint.RowPitch;
        for (UINT x = 0; x < kImageWidth; ++x) {
            const auto expected = pixel_value(x, y);
            if (std::memcmp(row + x * kPixelBytes, expected.data(), kPixelBytes) != 0) {
                accurate = false;
                break;
            }
        }
    }
    const D3D12_RANGE no_cpu_writes{0, 0};
    readback->Unmap(0, &no_cpu_writes);
    last_error_ = accurate ? S_OK : HRESULT_FROM_WIN32(ERROR_CRC);
    return last_error_;
}
} // namespace scarlett::gpu
