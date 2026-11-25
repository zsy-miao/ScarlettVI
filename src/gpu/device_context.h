#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <span>
#include <string>

namespace scarlett::gpu {
class GuestAddressSpace;

// Persistent, independent Windows D3D12 backend. This does not expose the
// Xbox D3D12_XS COM ABI; console vtables are translated in a separate layer.
class DeviceContext final {
public:
    DeviceContext();
    ~DeviceContext();
    DeviceContext(const DeviceContext&) = delete;
    DeviceContext& operator=(const DeviceContext&) = delete;

    HRESULT initialize();
    // Acquire the exact public ID3D12Device11 requested by the console title.
    // QueryInterface preserves native COM ownership; unsupported host runtimes
    // fail explicitly rather than fabricating a console vtable.
    HRESULT acquire_device11(ID3D12Device11** out) const;
    HRESULT texture_roundtrip_test();
    HRESULT device11_contract_test();
    // Real Windows D3D12 UpdateTileMappings on two large sparse GPU
    // resources backed by one 64KiB heap tile, verified by GPU readback.
    HRESULT tiled_buffer_alias_test();
    // Four independently indexed physical heap tiles are mapped to one
    // contiguous guest region per native D3D12 resource in one API call.
    HRESULT tile_batch_mapping_test();
    // Explicit 64KiB XMem CPU page -> real sparse GPU tile -> readback ->
    // XMem alias staging, with completed D3D12 fences. This is NOT coherent
    // unified console memory and does not emulate XMemTransferMemory.
    HRESULT staged_xmem_tile_roundtrip_test();
    // Validates four pinned guest XMem frames -> eight real GPU tile aliases
    // and host transfer/writeback, including atomic pin-safe unmap failures.
    HRESULT guest_gpu_page_registry_test();
    HRESULT guest_gpu_page_batch_test();
    uint64_t registry_batch_mapping_updates() const noexcept {
        return registry_batch_mapping_updates_;
    }
    uint64_t registry_grouped_unmapping_updates() const noexcept {
        return registry_grouped_unmapping_updates_;
    }
    // Copies generated format-aware RGBA/block-compressed texture mips
    // through real D3D12 GPU footprints and verifies every returned block.
    HRESULT texture_layout_gpu_test();
    HRESULT compute_dispatch_test(uint32_t seed);
    HRESULT texture_shader_test();
    // Submits host DXIL work between CPU-visible 64KiB console page views.
    // Guest GPU VAs are synthetic and translated through GuestAddressSpace.
    HRESULT execute_guest_buffer(std::span<const uint32_t> guest_input,
                                 std::span<uint32_t> guest_output,
                                 GuestAddressSpace& guest_gpu_va,
                                 uint64_t input_guest_base, uint64_t output_guest_base,
                                 uint32_t seed);
    uint32_t compiled_dxil_bytes() const noexcept { return compiled_dxil_bytes_; }
    uint32_t texture_dxil_bytes() const noexcept { return texture_dxil_bytes_; }
    const std::string& shader_diagnostics() const noexcept { return shader_diagnostics_; }
    const std::wstring& adapter_name() const noexcept { return adapter_name_; }
    HRESULT last_error() const noexcept { return last_error_; }
    bool ready() const noexcept { return device_ && queue_ && allocator_ && command_list_ && fence_ && fence_event_; }
    uint64_t submitted_commands() const noexcept { return submitted_; }
    uint64_t registry_queue_submissions() const noexcept {
        return registry_queue_submissions_;
    }
    uint64_t staged_completed_fence() const noexcept { return staged_completed_fence_; }
    uint64_t tile_mapping_updates() const noexcept { return tile_mapping_updates_; }
    uint64_t tile_unmapping_updates() const noexcept { return tile_unmapping_updates_; }
    uint64_t completed_fence() const noexcept { return fence_ ? fence_->GetCompletedValue() : 0; }

private:
    HRESULT create_buffer(D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES initial_state,
                          uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Resource>& output);
    HRESULT wait_for_gpu(uint64_t value);
    HRESULT prepare_compute_pipeline();
    HRESULT prepare_texture_pipeline();
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> compute_root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> compute_pipeline_;
    uint32_t compiled_dxil_bytes_ = 0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> texture_root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> texture_pipeline_;
    uint32_t texture_dxil_bytes_ = 0;
    std::string shader_diagnostics_;
    HANDLE fence_event_ = nullptr;
    std::wstring adapter_name_;
    uint64_t next_fence_ = 1;
    uint64_t submitted_ = 0;
    uint64_t registry_queue_submissions_ = 0;
    uint64_t registry_batch_mapping_updates_ = 0;
    uint64_t registry_grouped_unmapping_updates_ = 0;
    uint64_t staged_completed_fence_ = 0;
    uint64_t tile_mapping_updates_ = 0;
    uint64_t tile_unmapping_updates_ = 0;
    HRESULT last_error_ = S_OK;
};

} // namespace scarlett::gpu
