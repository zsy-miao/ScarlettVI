#include "host_backend.h"
#include <algorithm>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace scarlett::gpu {
using Microsoft::WRL::ComPtr;

std::vector<DeviceReport> probe_host_adapters() {
    std::vector<DeviceReport> reports;
    ComPtr<IDXGIFactory6> factory;
    const HRESULT factory_hr = CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(factory_hr)) {
        DeviceReport report;
        report.adapter_name = L"(DXGI factory creation failed)";
        report.result = factory_hr;
        reports.push_back(std::move(report));
        return reports;
    }

    for (UINT index = 0; index < 32; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enum_hr = factory->EnumAdapters1(index, adapter.GetAddressOf());
        if (enum_hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enum_hr)) break;

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;

        DeviceReport report;
        report.adapter_name = desc.Description;
        report.vendor_id = desc.VendorId;
        report.device_id = desc.DeviceId;
        report.dedicated_video_memory_bytes = desc.DedicatedVideoMemory;
        report.is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

        // A software adapter is not suitable for the target's real-time RDNA2
        // command stream. Report it, but do not count it as a usable device.
        if (report.is_software) {
            report.result = E_NOTIMPL;
            reports.push_back(std::move(report));
            continue;
        }

        ComPtr<ID3D12Device> device;
        HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                       IID_PPV_ARGS(device.GetAddressOf()));
        report.level_12_0 = SUCCEEDED(hr);
        if (FAILED(hr))
            hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(device.GetAddressOf()));
        report.result = hr;
        if (FAILED(hr)) {
            reports.push_back(std::move(report));
            continue;
        }
        report.device_created = true;
        report.node_count = device->GetNodeCount();

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        ComPtr<ID3D12CommandQueue> queue;
        hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue.GetAddressOf()));
        report.result = hr;
        if (FAILED(hr)) { reports.push_back(std::move(report)); continue; }
        report.queue_created = true;

        ComPtr<ID3D12CommandAllocator> allocator;
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(allocator.GetAddressOf()));
        if (FAILED(hr)) { report.result = hr; reports.push_back(std::move(report)); continue; }
        ComPtr<ID3D12GraphicsCommandList> list;
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                       IID_PPV_ARGS(list.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = list->Close();
        if (FAILED(hr)) { report.result = hr; reports.push_back(std::move(report)); continue; }
        report.command_list_created = true;

        ComPtr<ID3D12Fence> fence;
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
        if (FAILED(hr)) { report.result = hr; reports.push_back(std::move(report)); continue; }
        report.fence_created = true;

        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC resource_desc{};
        resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resource_desc.Width = 64 * 1024;
        resource_desc.Height = 1;
        resource_desc.DepthOrArraySize = 1;
        resource_desc.MipLevels = 1;
        resource_desc.SampleDesc.Count = 1;
        resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload_buffer;
        hr = device->CreateCommittedResource(
            &heap_props, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(upload_buffer.GetAddressOf()));
        if (FAILED(hr)) { report.result = hr; reports.push_back(std::move(report)); continue; }
        void* mapped = nullptr;
        const D3D12_RANGE empty_read_range{0, 0};
        hr = upload_buffer->Map(0, &empty_read_range, &mapped);
        if (SUCCEEDED(hr) && mapped) {
            auto* bytes = static_cast<uint8_t*>(mapped);
            bytes[0] = 0x53; // locally-generated upload data, no title assets
            bytes[1] = 0x56;
            const D3D12_RANGE written_range{0, 2};
            upload_buffer->Unmap(0, &written_range);
            report.upload_buffer_created = true;
        }
        if (FAILED(hr)) { report.result = hr; reports.push_back(std::move(report)); continue; }

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC signature{};
        signature.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        signature.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        hr = D3D12SerializeVersionedRootSignature(&signature,
                                                   serialized.GetAddressOf(),
                                                   errors.GetAddressOf());
        if (SUCCEEDED(hr)) {
            ComPtr<ID3D12RootSignature> root;
            hr = device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                             serialized->GetBufferSize(),
                                             IID_PPV_ARGS(root.GetAddressOf()));
            report.root_signature_created = SUCCEEDED(hr);
        }
        report.result = hr;
        reports.push_back(std::move(report));
    }
    return reports;
}
} // namespace scarlett::gpu
