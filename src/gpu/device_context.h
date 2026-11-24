#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

namespace scarlett::gpu {

// Persistent, independent Windows D3D12 backend. This does not expose the
// Xbox D3D12_XS COM ABI; console vtables are translated in a separate layer.
class DeviceContext final {
public:
    DeviceContext();
    ~DeviceContext();
    DeviceContext(const DeviceContext&) = delete;
    DeviceContext& operator=(const DeviceContext&) = delete;

    HRESULT initialize();
    HRESULT texture_roundtrip_test();
    HRESULT compute_dispatch_test(uint32_t seed);
    HRESULT texture_shader_test();
    uint32_t compiled_dxil_bytes() const noexcept { return compiled_dxil_bytes_; }
    uint32_t texture_dxil_bytes() const noexcept { return texture_dxil_bytes_; }
    const std::string& shader_diagnostics() const noexcept { return shader_diagnostics_; }
    const std::wstring& adapter_name() const noexcept { return adapter_name_; }
    HRESULT last_error() const noexcept { return last_error_; }
    bool ready() const noexcept { return device_ && queue_ && allocator_ && command_list_ && fence_ && fence_event_; }
    uint64_t submitted_commands() const noexcept { return submitted_; }
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
    HRESULT last_error_ = S_OK;
};

} // namespace scarlett::gpu
