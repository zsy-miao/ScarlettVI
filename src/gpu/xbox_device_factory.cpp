#include "xbox_device_factory.h"
#include "device_context.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <mutex>
#include <cstring>

namespace scarlett::gpu {
namespace {
struct HostDeviceSingleton {
    DeviceContext context;
    HRESULT initialize_status = E_UNEXPECTED;
    std::once_flag initialized;
};
HostDeviceSingleton& singleton() {
    static HostDeviceSingleton instance;
    return instance;
}
HRESULT ensure_host_device() {
    auto& host = singleton();
    std::call_once(host.initialized, [&host] {
        host.initialize_status = host.context.initialize();
    });
    return host.initialize_status;
}
}

HRESULT WINAPI xbox_create_device_host(
    void* adapter,
    const XboxDeviceParametersObserved* parameters,
    REFIID iid,
    void** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!parameters) return E_POINTER;
    // The supplied title calls with a null adapter. An Xbox adapter is NOT a
    // DXGI adapter and cannot be cast to IDXGIAdapter; fail explicitly.
    if (adapter) return E_NOTIMPL;
    if (!IsEqualGUID(iid, __uuidof(ID3D12Device11))) return E_NOINTERFACE;

    // These bytes are confirmed by the user's executable's initializer
    // (VA 0x14247858C and 0x142478592). All other Xbox config fields and
    // command-ring behavior remain untranslated; the process readiness gate
    // records that separate critical limitation.
    if (parameters->sdk_version != 0x00020004 ||
        parameters->graphics_queue_ring_bytes != 0x00100000)
        return E_NOTIMPL;

    HRESULT hr = ensure_host_device();
    if (FAILED(hr)) return hr;
    ID3D12Device11* device = nullptr;
    hr = singleton().context.acquire_device11(&device);
    if (SUCCEEDED(hr)) *output = device; // owns one COM reference
    return hr;
}

bool verify_xbox_device_factory() noexcept {
    XboxDeviceParametersObserved parameters{};
    parameters.sdk_version = 0x00020004;
    parameters.graphics_queue_ring_bytes = 0x00100000;
    parameters.unknown_0c_through_4f[0x28 - 0x0c] = 1;
    parameters.unknown_0c_through_4f[0x3c - 0x0c] = 0x20;
    void* output = reinterpret_cast<void*>(uintptr_t{1});
    if (xbox_create_device_host(nullptr, nullptr, __uuidof(ID3D12Device11), &output) != E_POINTER ||
        output != nullptr) return false;
    if (xbox_create_device_host(nullptr, &parameters, __uuidof(ID3D12Device11), nullptr) != E_POINTER)
        return false;
    output = reinterpret_cast<void*>(uintptr_t{1});
    if (xbox_create_device_host(reinterpret_cast<void*>(uintptr_t{1}), &parameters,
                                __uuidof(ID3D12Device11), &output) != E_NOTIMPL ||
        output) return false;
    if (xbox_create_device_host(nullptr, &parameters, __uuidof(ID3D12CommandQueue),
                                &output) != E_NOINTERFACE || output) return false;
    parameters.sdk_version = 0;
    if (xbox_create_device_host(nullptr, &parameters, __uuidof(ID3D12Device11),
                                &output) != E_NOTIMPL || output) return false;
    parameters.sdk_version = 0x00020004;
    parameters.graphics_queue_ring_bytes = 4096;
    if (xbox_create_device_host(nullptr, &parameters, __uuidof(ID3D12Device11),
                                &output) != E_NOTIMPL || output) return false;
    parameters.graphics_queue_ring_bytes = 0x00100000;
    Microsoft::WRL::ComPtr<ID3D12Device11> device;
    HRESULT hr = xbox_create_device_host(nullptr, &parameters,
               __uuidof(ID3D12Device11), reinterpret_cast<void**>(device.GetAddressOf()));
    if (FAILED(hr) || !device || device->GetNodeCount() == 0) return false;

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    hr = device->CreateCommandQueue(&queue_desc,
                                    IID_PPV_ARGS(queue.GetAddressOf()));
    if (FAILED(hr) || !queue ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return false;
    // A second request must return the same IUnknown COM identity and a
    // separately owned reference. It must never return a temporary pointer.
    Microsoft::WRL::ComPtr<ID3D12Device11> second;
    hr = xbox_create_device_host(nullptr, &parameters,
        __uuidof(ID3D12Device11), reinterpret_cast<void**>(second.GetAddressOf()));
    if (FAILED(hr) || !second) return false;
    Microsoft::WRL::ComPtr<IUnknown> a, b;
    if (FAILED(device.As(&a)) || FAILED(second.As(&b))) return false;
    return a.Get() == b.Get();
}
} // namespace scarlett::gpu
