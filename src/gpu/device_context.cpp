#include "device_context.h"
#include "dxil_compiler.h"
#include "guest_address_space.h"
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

HRESULT DeviceContext::acquire_device11(ID3D12Device11** out) const {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!ready()) return E_UNEXPECTED;
    return device_->QueryInterface(IID_PPV_ARGS(out));
}

HRESULT DeviceContext::device11_contract_test() {
    if (!ready()) {
        last_error_ = initialize();
        if (FAILED(last_error_)) return last_error_;
    }
    Microsoft::WRL::ComPtr<ID3D12Device11> device11;
    last_error_ = acquire_device11(device11.GetAddressOf());
    if (FAILED(last_error_)) return last_error_;

    // Verify interface identity and IUnknown's stable COM identity.
    Microsoft::WRL::ComPtr<IUnknown> original_identity;
    Microsoft::WRL::ComPtr<IUnknown> new_identity;
    last_error_ = device_->QueryInterface(IID_PPV_ARGS(original_identity.GetAddressOf()));
    if (FAILED(last_error_)) return last_error_;
    last_error_ = device11->QueryInterface(IID_PPV_ARGS(new_identity.GetAddressOf()));
    if (FAILED(last_error_)) return last_error_;
    if (original_identity.Get() != new_identity.Get()) {
        last_error_ = E_UNEXPECTED;
        return last_error_;
    }

    // Use the standard base device methods whose offsets are independently
    // seen in the user's title. A host device11 is NOT proof that all Xbox
    // D3D12_XS extensions, resource flags or GPU page-table policies work.
    if (device11->GetNodeCount() == 0) {
        last_error_ = E_FAIL;
        return last_error_;
    }
    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_description.NodeMask = 0;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    last_error_ = device11->CreateCommandQueue(
        &queue_description, IID_PPV_ARGS(queue.GetAddressOf()));
    if (FAILED(last_error_)) return last_error_;
    if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        last_error_ = E_FAIL;
        return last_error_;
    }

    constexpr uint64_t kTestBytes = 64 * 1024;
    auto resource_description = buffer_description(kTestBytes);
    const auto allocation = device11->GetResourceAllocationInfo(
        0, 1, &resource_description);
    if (allocation.SizeInBytes == UINT64_MAX ||
        allocation.SizeInBytes < kTestBytes ||
        allocation.Alignment < D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) {
        last_error_ = E_FAIL;
        return last_error_;
    }
    last_error_ = S_OK;
    return last_error_;
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

HRESULT DeviceContext::prepare_compute_pipeline() {
    if (compute_pipeline_) return S_OK;
    if (!ready()) {
        last_error_ = initialize();
        if (FAILED(last_error_)) return last_error_;
    }

    // Original SM6 compute shader. No Xbox ISA, proprietary shader asset,
    // precompiled bytecode or third-party shader source is used here.
    constexpr std::string_view shader = R"scarlett(
StructuredBuffer<uint> Inputs : register(t0);
RWStructuredBuffer<uint> Outputs : register(u0);
cbuffer Parameters : register(b0) {
    uint Count;
    uint Seed;
};
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i < Count) {
        Outputs[i] = (Inputs[i] ^ 0xA5A5A5A5u) + i * 7u + Seed;
    }
}
)scarlett";
    const DxilCompileResult compiled = compile_compute_dxil(shader);
    shader_diagnostics_ = compiled.diagnostics;
    if (FAILED(compiled.status)) {
        last_error_ = compiled.status;
        return last_error_;
    }

    // Root SRV/UAV descriptors are legal for structured buffer resources.
    // They avoid a descriptor heap for this minimal real dispatch backend.
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].Descriptor.RegisterSpace = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor.ShaderRegister = 0;
    parameters[1].Descriptor.RegisterSpace = 0;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[2].Constants.Num32BitValues = 2;
    parameters[2].Constants.ShaderRegister = 0;
    parameters[2].Constants.RegisterSpace = 0;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = static_cast<UINT>(std::size(parameters));
    signature.pParameters = parameters;
    signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    last_error_ = D3D12SerializeRootSignature(
        &signature, D3D_ROOT_SIGNATURE_VERSION_1,
        serialized.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(last_error_)) return last_error_;
    last_error_ = device_->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(compute_root_.ReleaseAndGetAddressOf()));
    if (FAILED(last_error_)) return last_error_;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = compute_root_.Get();
    pipeline.CS.pShaderBytecode = compiled.binary.data();
    pipeline.CS.BytecodeLength = compiled.binary.size();
    last_error_ = device_->CreateComputePipelineState(
        &pipeline, IID_PPV_ARGS(compute_pipeline_.ReleaseAndGetAddressOf()));
    if (SUCCEEDED(last_error_))
        compiled_dxil_bytes_ = static_cast<uint32_t>(compiled.binary.size());
    return last_error_;
}

HRESULT DeviceContext::compute_dispatch_test(uint32_t seed) {
    last_error_ = prepare_compute_pipeline();
    if (FAILED(last_error_)) return last_error_;
    constexpr uint32_t kElementCount = 1024;
    constexpr uint64_t kBufferBytes = sizeof(uint32_t) * kElementCount;
    constexpr UINT kThreadGroupSize = 64;

    Microsoft::WRL::ComPtr<ID3D12Resource> input, output, readback;
    last_error_ = create_buffer(D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ,
                                kBufferBytes, input);
    if (FAILED(last_error_)) return last_error_;

    // GPU unordered-access output; cannot use a CPU-visible upload heap for
    // writes. D3D12 COPY_SOURCE transition will make writes readable later.
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto writable_description = buffer_description(kBufferBytes);
    writable_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    last_error_ = device_->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &writable_description,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(output.ReleaseAndGetAddressOf()));
    if (FAILED(last_error_)) return last_error_;
    last_error_ = create_buffer(D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_STATE_COPY_DEST,
                                kBufferBytes, readback);
    if (FAILED(last_error_)) return last_error_;

    std::array<uint32_t, kElementCount> values{};
    for (uint32_t i = 0; i < kElementCount; ++i)
        values[i] = (i * 0x01010101u) ^ (i << 13) ^ 0xC0DEC0DEu;

    void* upload_data = nullptr;
    const D3D12_RANGE no_cpu_reads{0, 0};
    last_error_ = input->Map(0, &no_cpu_reads, &upload_data);
    if (FAILED(last_error_) || !upload_data) {
        last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
        return last_error_;
    }
    std::memcpy(upload_data, values.data(), kBufferBytes);
    const D3D12_RANGE written{0, static_cast<SIZE_T>(kBufferBytes)};
    input->Unmap(0, &written);

    // The same persistent queue, allocator, list and fence were used by the
    // texture-copy backend. Commands are submitted only after prior fence
    // completion, so allocator reset cannot overlap in-flight GPU work.
    last_error_ = allocator_->Reset();
    if (FAILED(last_error_)) return last_error_;
    last_error_ = command_list_->Reset(allocator_.Get(), compute_pipeline_.Get());
    if (FAILED(last_error_)) return last_error_;
    command_list_->SetComputeRootSignature(compute_root_.Get());
    command_list_->SetComputeRootShaderResourceView(0, input->GetGPUVirtualAddress());
    command_list_->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    const uint32_t constants[2]{kElementCount, seed};
    command_list_->SetComputeRoot32BitConstants(2, 2, constants, 0);
    command_list_->Dispatch(kElementCount / kThreadGroupSize, 1, 1);

    D3D12_RESOURCE_BARRIER transition{};
    transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    transition.Transition.pResource = output.Get();
    transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    command_list_->ResourceBarrier(1, &transition);
    command_list_->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, kBufferBytes);

    last_error_ = command_list_->Close();
    if (FAILED(last_error_)) return last_error_;
    ID3D12CommandList* submitted[]{command_list_.Get()};
    queue_->ExecuteCommandLists(1, submitted);
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

    void* readback_data = nullptr;
    const D3D12_RANGE cpu_reads{0, static_cast<SIZE_T>(kBufferBytes)};
    last_error_ = readback->Map(0, &cpu_reads, &readback_data);
    if (FAILED(last_error_) || !readback_data) {
        last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
        return last_error_;
    }
    const auto* observed = static_cast<const uint32_t*>(readback_data);
    bool matching = true;
    for (uint32_t i = 0; i < kElementCount; ++i) {
        const uint32_t expected = (values[i] ^ 0xA5A5A5A5u) + i * 7u + seed;
        if (observed[i] != expected) {
            matching = false;
            break;
        }
    }
    const D3D12_RANGE no_cpu_writes{0, 0};
    readback->Unmap(0, &no_cpu_writes);
    last_error_ = matching ? S_OK : HRESULT_FROM_WIN32(ERROR_CRC);
    return last_error_;
}

HRESULT DeviceContext::prepare_texture_pipeline() {
    if (texture_pipeline_) return S_OK;
    if (!ready()) {
        last_error_ = initialize();
        if (FAILED(last_error_)) return last_error_;
    }

    constexpr std::string_view source = R"scarlett(
Texture2D<float4> SourceTexture : register(t0);
RWTexture2D<float4> DestinationTexture : register(u0);
cbuffer Dimensions : register(b0) {
    uint Width;
    uint Height;
};
[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
    const uint2 xy = dispatch_id.xy;
    if (xy.x >= Width || xy.y >= Height) return;
    const float4 rgba = SourceTexture.Load(int3(xy, 0));
    DestinationTexture[xy] = float4(1.0 - rgba.rgb, rgba.a);
}
)scarlett";
    const auto compiled = compile_compute_dxil(source);
    shader_diagnostics_ = compiled.diagnostics;
    if (FAILED(compiled.status)) {
        last_error_ = compiled.status;
        return last_error_;
    }

    // Actual shader-visible descriptor-table binding, not direct GPU VA root
    // descriptors. Both texture descriptors share one consecutive table.
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 2;
    parameters[0].DescriptorTable.pDescriptorRanges = ranges;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0;
    parameters[1].Constants.Num32BitValues = 2;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = static_cast<UINT>(std::size(parameters));
    signature.pParameters = parameters;
    signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    last_error_ = D3D12SerializeRootSignature(
        &signature, D3D_ROOT_SIGNATURE_VERSION_1,
        serialized.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(last_error_)) return last_error_;
    last_error_ = device_->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(texture_root_.ReleaseAndGetAddressOf()));
    if (FAILED(last_error_)) return last_error_;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = texture_root_.Get();
    pipeline.CS.pShaderBytecode = compiled.binary.data();
    pipeline.CS.BytecodeLength = compiled.binary.size();
    last_error_ = device_->CreateComputePipelineState(
        &pipeline, IID_PPV_ARGS(texture_pipeline_.ReleaseAndGetAddressOf()));
    if (SUCCEEDED(last_error_))
        texture_dxil_bytes_ = static_cast<uint32_t>(compiled.binary.size());
    return last_error_;
}

HRESULT DeviceContext::texture_shader_test() {
    last_error_ = prepare_texture_pipeline();
    if (FAILED(last_error_)) return last_error_;

    // Ensure the host device really supports R8G8B8A8_UNORM typed UAV stores.
    // Never assume Xbox-specific texture formats are equivalent.
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    last_error_ = device_->CheckFeatureSupport(
        D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(last_error_)) return last_error_;
    if ((support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
        last_error_ = DXGI_ERROR_UNSUPPORTED;
        return last_error_;
    }

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = kImageWidth;
    description.Height = kImageHeight;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 bytes_per_row = 0, buffer_bytes = 0;
    device_->GetCopyableFootprints(&description, 0, 1, 0, &footprint,
                                   &rows, &bytes_per_row, &buffer_bytes);
    if (rows != kImageHeight || bytes_per_row != kImageWidth * kPixelBytes ||
        buffer_bytes > 32ull * 1024ull * 1024ull ||
        buffer_bytes == 0 || footprint.Footprint.RowPitch < bytes_per_row) {
        last_error_ = E_INVALIDARG;
        return last_error_;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> upload, texture_input;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture_output, readback;
    last_error_ = create_buffer(D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ,
                                buffer_bytes, upload);
    if (FAILED(last_error_)) return last_error_;
    last_error_ = create_buffer(D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_STATE_COPY_DEST,
                                buffer_bytes, readback);
    if (FAILED(last_error_)) return last_error_;

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    last_error_ = device_->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(texture_input.ReleaseAndGetAddressOf()));
    if (FAILED(last_error_)) return last_error_;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    last_error_ = device_->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(texture_output.ReleaseAndGetAddressOf()));
    if (FAILED(last_error_)) return last_error_;

    D3D12_DESCRIPTOR_HEAP_DESC heap_info{};
    heap_info.NumDescriptors = 2;
    heap_info.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_info.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    last_error_ = device_->CreateDescriptorHeap(
        &heap_info, IID_PPV_ARGS(descriptor_heap.GetAddressOf()));
    if (FAILED(last_error_)) return last_error_;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    const auto first_handle = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    device_->CreateShaderResourceView(texture_input.Get(), &srv, first_handle);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    auto second_handle = first_handle;
    second_handle.ptr += device_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device_->CreateUnorderedAccessView(
        texture_output.Get(), nullptr, &uav, second_handle);

    void* upload_data = nullptr;
    const D3D12_RANGE no_read{0, 0};
    last_error_ = upload->Map(0, &no_read, &upload_data);
    if (FAILED(last_error_) || !upload_data) {
        last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
        return last_error_;
    }
    std::memset(upload_data, 0, static_cast<size_t>(buffer_bytes));
    for (UINT y = 0; y < kImageHeight; ++y) {
        auto* row = static_cast<uint8_t*>(upload_data) + footprint.Offset +
                    static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < kImageWidth; ++x) {
            const auto pixel = pixel_value(x, y);
            std::memcpy(row + x * kPixelBytes, pixel.data(), pixel.size());
        }
    }
    const D3D12_RANGE written{0, static_cast<SIZE_T>(buffer_bytes)};
    upload->Unmap(0, &written);

    last_error_ = allocator_->Reset();
    if (FAILED(last_error_)) return last_error_;
    last_error_ = command_list_->Reset(allocator_.Get(), texture_pipeline_.Get());
    if (FAILED(last_error_)) return last_error_;

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION gpu_input{};
    gpu_input.pResource = texture_input.Get();
    gpu_input.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    command_list_->CopyTextureRegion(&gpu_input, 0, 0, 0, &source, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture_input.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    command_list_->ResourceBarrier(1, &barrier);

    ID3D12DescriptorHeap* const heaps[]{descriptor_heap.Get()};
    command_list_->SetDescriptorHeaps(1, heaps);
    command_list_->SetComputeRootSignature(texture_root_.Get());
    command_list_->SetComputeRootDescriptorTable(
        0, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
    const uint32_t constants[2]{kImageWidth, kImageHeight};
    command_list_->SetComputeRoot32BitConstants(1, 2, constants, 0);
    command_list_->Dispatch((kImageWidth + 7) / 8, (kImageHeight + 7) / 8, 1);

    barrier.Transition.pResource = texture_output.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    command_list_->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION gpu_output{};
    gpu_output.pResource = texture_output.Get();
    gpu_output.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    command_list_->CopyTextureRegion(&destination, 0, 0, 0, &gpu_output, nullptr);
    last_error_ = command_list_->Close();
    if (FAILED(last_error_)) return last_error_;

    ID3D12CommandList* submitted[]{command_list_.Get()};
    queue_->ExecuteCommandLists(1, submitted);
    ++submitted_;
    const uint64_t value = next_fence_++;
    last_error_ = queue_->Signal(fence_.Get(), value);
    if (FAILED(last_error_)) return last_error_;
    last_error_ = wait_for_gpu(value);
    if (FAILED(last_error_)) {
        const HRESULT removed = device_->GetDeviceRemovedReason();
        if (FAILED(removed)) last_error_ = removed;
        return last_error_;
    }
    void* readback_data = nullptr;
    const D3D12_RANGE cpu_read{0, static_cast<SIZE_T>(buffer_bytes)};
    last_error_ = readback->Map(0, &cpu_read, &readback_data);
    if (FAILED(last_error_) || !readback_data) {
        last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
        return last_error_;
    }
    bool correct = true;
    for (UINT y = 0; y < kImageHeight && correct; ++y) {
        const auto* row = static_cast<const uint8_t*>(readback_data) + footprint.Offset +
                          static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < kImageWidth; ++x) {
            const auto expected = pixel_value(x, y);
            const uint8_t result[4]{
                static_cast<uint8_t>(255 - expected[0]),
                static_cast<uint8_t>(255 - expected[1]),
                static_cast<uint8_t>(255 - expected[2]),
                expected[3]
            };
            if (std::memcmp(row + x * kPixelBytes, result, kPixelBytes) != 0) {
                correct = false;
                break;
            }
        }
    }
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    last_error_ = correct ? S_OK : HRESULT_FROM_WIN32(ERROR_CRC);
    return last_error_;
}
HRESULT DeviceContext::execute_guest_buffer(std::span<const uint32_t> guest_input,
                                            std::span<uint32_t> guest_output,
                                            GuestAddressSpace& address_space,
                                            uint64_t guest_input_base,
                                            uint64_t guest_output_base,
                                            uint32_t seed) {
    constexpr uint64_t kPage = GuestAddressSpace::guest_alignment;
    constexpr uint64_t kInputOffset = 256;
    constexpr uint64_t kOutputOffset = 512;
    if (guest_input.empty() || guest_input.size() != guest_output.size() ||
        guest_input.size() > (kPage - kOutputOffset) / sizeof(uint32_t) ||
        guest_input.size() > UINT32_MAX) {
        last_error_ = E_INVALIDARG;
        return last_error_;
    }
    last_error_ = prepare_compute_pipeline();
    if (FAILED(last_error_)) return last_error_;
    const uint32_t count = static_cast<uint32_t>(guest_input.size());
    const uint64_t bytes = count * sizeof(uint32_t);

    Microsoft::WRL::ComPtr<ID3D12Resource> input, output, readback;
    last_error_ = create_buffer(D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ, kPage, input);
    if (FAILED(last_error_)) return last_error_;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto uav_description = buffer_description(kPage);
    uav_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    last_error_ = device_->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &uav_description,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(output.ReleaseAndGetAddressOf()));
    if (FAILED(last_error_)) return last_error_;
    last_error_ = create_buffer(D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_STATE_COPY_DEST, kPage, readback);
    if (FAILED(last_error_)) return last_error_;

    // This registry translates original synthetic 64 KiB guest ranges into
    // real D3D12 GPU virtual addresses. Never pass Xbox GPU page numbers to
    // a Windows D3D12 API as though they were host allocations.
    if (!address_space.register_buffer(guest_input_base, kPage, input.Get())) {
        last_error_ = E_INVALIDARG;
        return last_error_;
    }
    if (!address_space.register_buffer(guest_output_base, kPage, output.Get())) {
        address_space.unregister_buffer(guest_input_base);
        last_error_ = E_INVALIDARG;
        return last_error_;
    }
    struct UnregisterGuard {
        GuestAddressSpace& space;
        uint64_t input_base;
        uint64_t output_base;
        ~UnregisterGuard() {
            space.unregister_buffer(input_base);
            space.unregister_buffer(output_base);
        }
    } guard{address_space, guest_input_base, guest_output_base};

    const auto src = address_space.resolve(guest_input_base + kInputOffset, bytes);
    const auto dst = address_space.resolve(guest_output_base + kOutputOffset, bytes);
    if (!src || !dst || src->resource.Get() != input.Get() ||
        dst->resource.Get() != output.Get() ||
        address_space.resolve(guest_input_base + kPage - 4, 8)) {
        last_error_ = E_FAIL;
        return last_error_;
    }
    void* upload_data = nullptr;
    const D3D12_RANGE no_cpu_read{0, 0};
    last_error_ = input->Map(0, &no_cpu_read, &upload_data);
    if (FAILED(last_error_) || !upload_data) {
        last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
        return last_error_;
    }
    std::memcpy(static_cast<uint8_t*>(upload_data) + kInputOffset,
                guest_input.data(), static_cast<size_t>(bytes));
    const D3D12_RANGE wrote{static_cast<SIZE_T>(kInputOffset),
                            static_cast<SIZE_T>(kInputOffset + bytes)};
    input->Unmap(0, &wrote);

    last_error_ = allocator_->Reset();
    if (FAILED(last_error_)) return last_error_;
    last_error_ = command_list_->Reset(allocator_.Get(), compute_pipeline_.Get());
    if (FAILED(last_error_)) return last_error_;
    command_list_->SetComputeRootSignature(compute_root_.Get());
    command_list_->SetComputeRootShaderResourceView(0, src->host_gpu_va);
    command_list_->SetComputeRootUnorderedAccessView(1, dst->host_gpu_va);
    const uint32_t constants[]{count, seed};
    command_list_->SetComputeRoot32BitConstants(2, 2, constants, 0);
    command_list_->Dispatch((count + 63) / 64, 1, 1);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    command_list_->ResourceBarrier(1, &barrier);
    command_list_->CopyBufferRegion(readback.Get(), 0,
                                    output.Get(), kOutputOffset, bytes);
    last_error_ = command_list_->Close();
    if (FAILED(last_error_)) return last_error_;
    ID3D12CommandList* const submitted[]{command_list_.Get()};
    queue_->ExecuteCommandLists(1, submitted);
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

    void* readback_data = nullptr;
    const D3D12_RANGE cpu_reads{0, static_cast<SIZE_T>(bytes)};
    last_error_ = readback->Map(0, &cpu_reads, &readback_data);
    if (FAILED(last_error_) || !readback_data) {
        last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
        return last_error_;
    }
    std::memcpy(guest_output.data(), readback_data, static_cast<size_t>(bytes));
    const D3D12_RANGE no_cpu_write{0, 0};
    readback->Unmap(0, &no_cpu_write);
    last_error_ = S_OK;
    return last_error_;
}

} // namespace scarlett::gpu
