#include "device_context.h"
#include "dxil_compiler.h"
#include "texture_layout.h"
#include "../runtime/xmem_bridge.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace scarlett::gpu {
namespace {
using Microsoft::WRL::ComPtr;
constexpr uint64_t kMaxGpuStagingBytes = 32ull * 1024 * 1024;
constexpr uint32_t kGpuThreadGroupSize = 64;
uint64_t aligned_dword(uint64_t value) noexcept {
    return (value + 3ull) & ~uint64_t{3};
}
bool overlaps(const uint8_t* a, size_t a_size,
              const uint8_t* b, size_t b_size) noexcept {
    if (!a_size || !b_size) return false;
    const uintptr_t x = reinterpret_cast<uintptr_t>(a);
    const uintptr_t y = reinterpret_cast<uintptr_t>(b);
    return x <= y ? y-x < a_size : x-y < b_size;
}
D3D12_RESOURCE_DESC storage_buffer(uint64_t bytes) noexcept {
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    result.Alignment = 0;
    result.Width = bytes;
    result.Height = 1;
    result.DepthOrArraySize = 1;
    result.MipLevels = 1;
    result.Format = DXGI_FORMAT_UNKNOWN;
    result.SampleDesc.Count = 1;
    result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    result.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return result;
}
} // namespace

HRESULT DeviceContext::prepare_texture_detile_pipeline() {
    if (detile_pipeline_) return S_OK;
    if (!ready()) {
        last_error_ = initialize();
        if (FAILED(last_error_)) return last_error_;
    }
    // The host-owned 8x8 Morton layout is NOT an Xbox texture swizzle.
    // The GPU resolves each byte from a padded tile input and writes
    // one ALIGNED DWORD per thread, including R8 subresource tail bytes.
    constexpr std::string_view source = R"scarlett(
ByteAddressBuffer Source : register(t0);
RWByteAddressBuffer Destination : register(u0);
cbuffer Layout : register(b0) {
    uint BlockWidth;
    uint BytesPerBlock;
    uint RowBytes;
    uint TiledOffset;
    uint OutputOffset;
    uint LinearBytes;
    uint TileColumns;
};
uint Morton8(uint x, uint y) {
    return (x & 1u) | ((y & 1u) << 1u)
         | ((x & 2u) << 1u) | ((y & 2u) << 2u)
         | ((x & 4u) << 2u) | ((y & 4u) << 3u);
}
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint local = id.x * 4u;
    if (local >= LinearBytes) return;
    uint packed = 0u;
    [unroll]
    for (uint lane = 0u; lane < 4u; ++lane) {
        uint byte_index = local + lane;
        if (byte_index >= LinearBytes) continue;
        uint block_index = byte_index / BytesPerBlock;
        uint bx = block_index % BlockWidth;
        uint by = block_index / BlockWidth;
        uint tile = (by / 8u) * TileColumns + (bx / 8u);
        uint tile_index = tile * 64u + Morton8(bx & 7u, by & 7u);
        uint byte_offset = TiledOffset +
            tile_index * BytesPerBlock + (byte_index % BytesPerBlock);
        uint encoded = Source.Load(byte_offset & ~3u);
        uint one_byte = (encoded >> ((byte_offset & 3u) * 8u)) & 255u;
        packed |= one_byte << (lane * 8u);
    }
    Destination.Store(OutputOffset + local, packed);
}
)scarlett";
    const DxilCompileResult shader = compile_compute_dxil(source);
    shader_diagnostics_ = shader.diagnostics;
    last_error_ = shader.status;
    if (FAILED(last_error_)) return last_error_;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;
    params[2].Constants.Num32BitValues = 7;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(std::size(params));
    desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> bytes;
    ComPtr<ID3DBlob> errors;
    last_error_ = D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1,
        bytes.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(last_error_)) return last_error_;
    last_error_ = device_->CreateRootSignature(
        0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
        IID_PPV_ARGS(detile_root_.ReleaseAndGetAddressOf()));
    if (FAILED(last_error_)) return last_error_;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = detile_root_.Get();
    pipeline.CS.pShaderBytecode = shader.binary.data();
    pipeline.CS.BytecodeLength = shader.binary.size();
    last_error_ = device_->CreateComputePipelineState(
        &pipeline, IID_PPV_ARGS(detile_pipeline_.ReleaseAndGetAddressOf()));
    if (SUCCEEDED(last_error_))
        detile_dxil_bytes_ = static_cast<uint32_t>(shader.binary.size());
    return last_error_;
}

HRESULT DeviceContext::detile_host_texture_gpu(
    const TextureLayout& layout,
    std::span<const uint8_t> tiled,
    std::span<uint8_t> linear) {
    if (!layout.linear_size() || !layout.tiled_size() ||
        layout.subresources().empty() ||
        layout.linear_size() > kMaxGpuStagingBytes ||
        layout.tiled_size() > kMaxGpuStagingBytes ||
        tiled.size() != layout.tiled_size() ||
        linear.size() != layout.linear_size() ||
        overlaps(tiled.data(), tiled.size(), linear.data(), linear.size()))
        return last_error_ = E_INVALIDARG;

    // Per-mip GPU output addresses are DWORD aligned. A tightly packed
    // R8 CPU mip may end at an odd byte count, so one GPU word must
    // never straddle a different mip's output or overwrite its first byte.
    std::vector<uint32_t> gpu_offsets;
    uint64_t output_bytes = 0;
    try {
        gpu_offsets.reserve(layout.subresources().size());
        for (const auto& sub : layout.subresources()) {
            if (!sub.block_width || !sub.block_height ||
                !sub.bytes_per_block || !sub.linear_bytes ||
                sub.linear_offset + sub.linear_bytes > layout.linear_size() ||
                sub.tiled_offset + sub.tiled_bytes > layout.tiled_size() ||
                output_bytes > kMaxGpuStagingBytes ||
                aligned_dword(sub.linear_bytes) >
                    kMaxGpuStagingBytes - output_bytes)
                return last_error_ = E_INVALIDARG;
            gpu_offsets.push_back(static_cast<uint32_t>(output_bytes));
            output_bytes += aligned_dword(sub.linear_bytes);
        }
    } catch (...) {
        return last_error_ = E_OUTOFMEMORY;
    }

    last_error_ = prepare_texture_detile_pipeline();
    if (FAILED(last_error_)) return last_error_;

    ComPtr<ID3D12Resource> input;
    ComPtr<ID3D12Resource> output;
    ComPtr<ID3D12Resource> readback;
    const uint64_t input_bytes = aligned_dword(layout.tiled_size());
    last_error_ = create_buffer(
        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
        input_bytes, input);
    if (FAILED(last_error_)) return last_error_;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto desc = storage_buffer(output_bytes);
    last_error_ = device_->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(output.ReleaseAndGetAddressOf()));
    if (FAILED(last_error_)) return last_error_;
    last_error_ = create_buffer(D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_STATE_COPY_DEST,
                                output_bytes, readback);
    if (FAILED(last_error_)) return last_error_;

    void* mapped = nullptr;
    const D3D12_RANGE no_read{0, 0};
    last_error_ = input->Map(0, &no_read, &mapped);
    if (FAILED(last_error_) || !mapped)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
    std::memcpy(mapped, tiled.data(), tiled.size());
    if (input_bytes > tiled.size())
        std::memset(static_cast<uint8_t*>(mapped) + tiled.size(), 0,
                    static_cast<size_t>(input_bytes - tiled.size()));
    const D3D12_RANGE wrote{0, static_cast<SIZE_T>(input_bytes)};
    input->Unmap(0, &wrote);

    last_error_ = allocator_->Reset();
    if (FAILED(last_error_)) return last_error_;
    last_error_ = command_list_->Reset(allocator_.Get(),
                                       detile_pipeline_.Get());
    if (FAILED(last_error_)) return last_error_;
    command_list_->SetComputeRootSignature(detile_root_.Get());
    command_list_->SetComputeRootShaderResourceView(
        0, input->GetGPUVirtualAddress());
    command_list_->SetComputeRootUnorderedAccessView(
        1, output->GetGPUVirtualAddress());

    for (size_t i = 0; i < layout.subresources().size(); ++i) {
        const auto& s = layout.subresources()[i];
        const uint32_t args[7]{
            s.block_width, s.bytes_per_block, s.row_bytes,
            static_cast<uint32_t>(s.tiled_offset), gpu_offsets[i],
            static_cast<uint32_t>(s.linear_bytes),
            (s.block_width + 7u) / 8u
        };
        command_list_->SetComputeRoot32BitConstants(2, 7, args, 0);
        const uint64_t words = aligned_dword(s.linear_bytes) / 4;
        command_list_->Dispatch(
            static_cast<UINT>((words + kGpuThreadGroupSize - 1) /
                              kGpuThreadGroupSize), 1, 1);
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    command_list_->ResourceBarrier(1, &barrier);
    command_list_->CopyBufferRegion(
        readback.Get(), 0, output.Get(), 0, output_bytes);
    last_error_ = command_list_->Close();
    if (FAILED(last_error_)) return last_error_;
    ID3D12CommandList* commands[]{command_list_.Get()};
    queue_->ExecuteCommandLists(1, commands);
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

    mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(output_bytes)};
    last_error_ = readback->Map(0, &read_range, &mapped);
    if (FAILED(last_error_) || !mapped)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
    // Only write to user output after *all* readback processing succeeded.
    const auto* gpu = static_cast<const uint8_t*>(mapped);
    for (size_t i = 0; i < layout.subresources().size(); ++i) {
        const auto& sub = layout.subresources()[i];
        std::memcpy(linear.data() + static_cast<size_t>(sub.linear_offset),
                    gpu + gpu_offsets[i],
                    static_cast<size_t>(sub.linear_bytes));
    }
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    return last_error_ = S_OK;
}

HRESULT DeviceContext::detile_host_texture_xmem_heap(
    const TextureLayout& layout, const void* tiled_source,
    void* linear_destination) {
    // Explicit host-only bridge for live ScarlettVI XMemAlloc allocations.
    // Do not read either supplied address before registry validation.
    constexpr uint64_t kMaxXMemBridgeBytes = 1024ull * 1024;
    if (!tiled_source || !linear_destination ||
        !layout.tiled_size() || !layout.linear_size() ||
        layout.tiled_size() > kMaxXMemBridgeBytes ||
        layout.linear_size() > kMaxXMemBridgeBytes ||
        layout.subresources().empty())
        return last_error_ = E_INVALIDARG;

    std::shared_ptr<xmem::HostXMemWritePin> output;
    const HRESULT pin_hr = xmem::pin_owned_xmem_destination(
        linear_destination, static_cast<size_t>(layout.linear_size()), output);
    if (FAILED(pin_hr)) return last_error_ = pin_hr;

    try {
        std::vector<uint8_t> tiled(static_cast<size_t>(layout.tiled_size()));
        std::vector<uint8_t> linear(static_cast<size_t>(layout.linear_size()));
        if (!xmem::snapshot_owned_xmem_allocation(
                tiled_source, tiled.size(), tiled.data()))
            return last_error_ = E_INVALIDARG;

        // All GPU operations use private host staging bytes. Only
        // successful fence/readback results are committed to the
        // independently pinned output allocation.
        const HRESULT gpu_hr = detile_host_texture_gpu(layout, tiled, linear);
        if (FAILED(gpu_hr)) return last_error_ = gpu_hr;
        return last_error_ = output->write(linear.data(), linear.size());
    } catch (...) {
        return last_error_ = E_OUTOFMEMORY;
    }
}

HRESULT DeviceContext::detile_host_texture_gpu_fixture() {
    // Generated synthetic 8x8 host Morton layout. This test drives actual
    // HLSL -> DXIL compilation, D3D12 GPU execution, fence and readback.
    uint32_t verified_cases = 0;
    for (DXGI_FORMAT format : kHostTextureFormats) {
        for (const auto [width, height] :
             {std::pair{1u, 1u}, std::pair{7u, 9u},
              std::pair{53u, 37u}, std::pair{129u, 65u}}) {
            const TextureDescription shape{
                width, height, width == 1u ? 1u : 4u, 2u, format};
            const auto layout = TextureLayout::build(shape);
            if (!layout)
                return last_error_ = E_FAIL;
            std::vector<uint8_t> expected(layout->linear_size());
            std::vector<uint8_t> tiled(layout->tiled_size());
            std::vector<uint8_t> output(layout->linear_size() + 16, 0xE7);
            for (size_t i = 0; i < expected.size(); ++i)
                expected[i] = static_cast<uint8_t>(
                    (i * 73u + (i >> 2u) * 17u +
                     width * 31u + height * 7u + uint32_t(format)) & 255u);
            if (!layout->swizzle(expected, tiled))
                return last_error_ = E_FAIL;

            const auto target =
                std::span<uint8_t>(output.data(), expected.size());
            const uint64_t prior_submissions = submitted_;
            if (detile_host_texture_gpu(*layout, std::span<const uint8_t>{}, target)
                != E_INVALIDARG)
                return last_error_ = E_FAIL;
            if (detile_host_texture_gpu(
                    *layout, tiled, target.first(target.size() - 1))
                != E_INVALIDARG)
                return last_error_ = E_FAIL;
            if (submitted_ != prior_submissions ||
                !std::all_of(output.begin(), output.end(),
                             [](uint8_t byte) {return byte == 0xE7;}))
                return last_error_ = E_FAIL;

            last_error_ = detile_host_texture_gpu(*layout, tiled, target);
            if (FAILED(last_error_)) return last_error_;
            if (!std::equal(expected.begin(), expected.end(),
                            output.begin()) ||
                !std::all_of(output.begin() + expected.size(),
                             output.end(),
                             [](uint8_t byte) {return byte == 0xE7;}))
                return last_error_ = HRESULT_FROM_WIN32(ERROR_CRC);
            ++verified_cases;
        }
    }
    if (verified_cases != 4u * kHostTextureFormats.size() ||
        !detile_dxil_bytes_)
        return last_error_ = E_FAIL;
    return last_error_ = S_OK;
}
HRESULT DeviceContext::detile_host_texture_xmem_fixture() {
    // Integration: generated guest-owned heap bytes -> real D3D12 DXIL
    // detile -> pinned guest-owned heap destination. No title assets.
    struct HeapOwner {
        void* pointer = nullptr;
        ~HeapOwner() { if (pointer) xmem::XMemFree(pointer, 0); }
    };
    uint32_t verified = 0;
    for (const DXGI_FORMAT format : kHostTextureFormats) {
        for (const auto [width, height] :
             {std::pair{1u, 1u}, std::pair{53u, 37u}}) {
            const auto layout = TextureLayout::build({
                width, height, width == 1 ? 1u : 4u, 2u, format});
            if (!layout || layout->tiled_size() > 1024 * 1024 ||
                layout->linear_size() > 1024 * 1024)
                return last_error_ = E_FAIL;
            std::vector<uint8_t> linear(layout->linear_size());
            std::vector<uint8_t> tiled(layout->tiled_size());
            for (size_t i = 0; i < linear.size(); ++i)
                linear[i] = static_cast<uint8_t>(
                    (i * 79u + (i >> 4) * 13u + uint32_t(format) +
                     width * 11u) & 255u);
            if (!layout->swizzle(linear, tiled))
                return last_error_ = E_FAIL;

            HeapOwner source{xmem::XMemAlloc(tiled.size() + 32, 0)};
            HeapOwner destination{xmem::XMemAlloc(linear.size() + 48, 0)};
            if (!source.pointer || !destination.pointer)
                return last_error_ = E_OUTOFMEMORY;
            auto* source_bytes = static_cast<uint8_t*>(source.pointer) + 13;
            auto* dest_bytes = static_cast<uint8_t*>(destination.pointer) + 16;
            std::memcpy(source_bytes, tiled.data(), tiled.size());
            std::memset(destination.pointer, 0xa3, linear.size() + 48);
            const uint64_t before_dispatch = submitted_;
            const size_t before_pins = xmem::active_xmem_write_pin_count();

            // Foreign source/destination pointers are never dereferenced.
            if (detile_host_texture_xmem_heap(
                    *layout, reinterpret_cast<void*>(uintptr_t{0x1234}),
                    dest_bytes) != E_INVALIDARG ||
                SUCCEEDED(detile_host_texture_xmem_heap(
                    *layout, source_bytes,
                    reinterpret_cast<void*>(uintptr_t{0x3456}))) ||
                submitted_ != before_dispatch ||
                xmem::active_xmem_write_pin_count() != before_pins ||
                !std::all_of(dest_bytes, dest_bytes + linear.size(),
                    [](uint8_t x) { return x == 0xa3; }))
                return last_error_ = E_FAIL;

            std::shared_ptr<xmem::HostXMemWritePin> retained;
            if (xmem::pin_owned_xmem_destination(
                    dest_bytes, linear.size(), retained) != S_OK ||
                xmem::active_xmem_write_pin_count() != before_pins + 1)
                return last_error_ = E_FAIL;

            last_error_ = detile_host_texture_xmem_heap(
                *layout, source_bytes, dest_bytes);
            if (FAILED(last_error_) ||
                submitted_ != before_dispatch + 1 ||
                xmem::active_xmem_write_pin_count() != before_pins + 1 ||
                !std::equal(linear.begin(), linear.end(), dest_bytes))
                return last_error_ = E_FAIL;
            const auto* canaries = static_cast<const uint8_t*>(
                destination.pointer);
            if (!std::all_of(canaries, canaries + 16,
                    [](uint8_t c) { return c == 0xa3; }) ||
                !std::all_of(canaries + 16 + linear.size(),
                             canaries + linear.size() + 48,
                    [](uint8_t c) { return c == 0xa3; }))
                return last_error_ = E_FAIL;
            // A caller cannot release output while an independent pin
            // remains, even after our internal synchronous pin retires.
            SetLastError(ERROR_SUCCESS);
            xmem::XMemFree(destination.pointer, 0);
            if (GetLastError() != ERROR_BUSY)
                return last_error_ = E_FAIL;
            retained.reset();
            if (xmem::active_xmem_write_pin_count() != before_pins)
                return last_error_ = E_FAIL;
            ++verified;
        }
    }
    // Oversize output is rejected without touching guest memory, or
    // issuing GPU work. That limit is a deliberate host policy.
    const auto oversize = TextureLayout::build({
        2048, 2048, 1, 1, DXGI_FORMAT_R8_UNORM});
    const uint64_t previous = submitted_;
    if (!oversize ||
        detile_host_texture_xmem_heap(
            *oversize, reinterpret_cast<const void*>(uintptr_t{0x1234}),
            reinterpret_cast<void*>(uintptr_t{0x3456})) != E_INVALIDARG ||
        submitted_ != previous ||
        verified != 2u * kHostTextureFormats.size())
        return last_error_ = E_FAIL;
    return last_error_ = S_OK;
}

} // namespace scarlett::gpu
