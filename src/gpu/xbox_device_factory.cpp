#include "xbox_device_factory.h"
#include "device_context.h"
#include "sparse_gpu_windows.h"
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
    std::mutex gpu_va_mutex;
    SparseGpuWindows gpu_windows;
    uint32_t configured_mode = 0;
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

// The title's one-argument Xbox configuration interface is NOT a Windows
// D3D12 export. This original subset reserves real host GPU virtual address
// windows, checks GPU VA capabilities, and persists them until shutdown.
// It never claims console tile mappings or Xbox-only GPU page protections.
HRESULT WINAPI xbox_configure_virtual_memory_host(
    const XboxGpuVaConfigurationObserved* configuration) noexcept {
    if (!configuration) return E_POINTER;
    const uint32_t mode = configuration->mode_04;
    if (configuration->unused_00 != 0 || (mode != 3 && mode != 4))
        return E_NOTIMPL;
    for (const auto other : configuration->other_08_through_1c)
        if (other != 0) return E_NOTIMPL;

    const HRESULT init_hr = ensure_host_device();
    if (FAILED(init_hr)) return init_hr;
    auto& host = singleton();
    std::lock_guard lock(host.gpu_va_mutex);
    if (host.configured_mode != 0)
        return host.configured_mode == mode
            ? S_OK : HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);

    Microsoft::WRL::ComPtr<ID3D12Device11> device;
    const HRESULT hr = host.context.acquire_device11(device.GetAddressOf());
    if (FAILED(hr)) return hr;
    const uint64_t window_bytes =
        (mode == 3 ? 16ull : 24ull) * 1024 * 1024 * 1024;
    const HRESULT reservation_hr =
        host.gpu_windows.initialize(device.Get(), window_bytes, 2);
    if (FAILED(reservation_hr)) return reservation_hr;
    if (!host.gpu_windows.verify_boundaries()) {
        host.gpu_windows.clear();
        return E_FAIL;
    }
    host.configured_mode = mode; // publish only after successful real work
    return S_OK;
}

bool verify_xbox_gpu_virtual_memory_configuration(uint32_t target_mode) noexcept {
    if (target_mode != 3 && target_mode != 4) return false;
    XboxGpuVaConfigurationObserved config{};
    if (xbox_configure_virtual_memory_host(nullptr) != E_POINTER)
        return false;
    config.mode_04 = 5;
    if (xbox_configure_virtual_memory_host(&config) != E_NOTIMPL)
        return false;
    config.mode_04 = target_mode;
    config.other_08_through_1c[2] = 1;
    if (xbox_configure_virtual_memory_host(&config) != E_NOTIMPL)
        return false;
    config.other_08_through_1c[2] = 0;
    if (FAILED(xbox_configure_virtual_memory_host(&config)) ||
        FAILED(xbox_configure_virtual_memory_host(&config)))
        return false;

    auto& host = singleton();
    {
        std::lock_guard lock(host.gpu_va_mutex);
        if (host.configured_mode != target_mode ||
            host.gpu_windows.window_count() != 2 ||
            host.gpu_windows.segment_count() !=
                (target_mode == 3 ? 8u : 12u) ||
            host.gpu_windows.window_bytes() !=
                (target_mode == 3 ? 16ull : 24ull) * 1024 * 1024 * 1024 ||
            !host.gpu_windows.verify_boundaries())
            return false;
    }
    config.mode_04 = target_mode == 3 ? 4 : 3;
    if (xbox_configure_virtual_memory_host(&config) !=
        HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        return false;
    {
        std::lock_guard lock(host.gpu_va_mutex);
        if (host.configured_mode != target_mode ||
            !host.gpu_windows.verify_boundaries())
            return false;
    }
    // The configured windows and device must share one real D3D12 device.
    XboxDeviceParametersObserved parameters{};
    parameters.sdk_version = 0x20004;
    parameters.graphics_queue_ring_bytes = 0x100000;
    Microsoft::WRL::ComPtr<ID3D12Device11> native_device;
    const HRESULT hr = xbox_create_device_host(
        nullptr, &parameters, __uuidof(ID3D12Device11),
        reinterpret_cast<void**>(native_device.GetAddressOf()));
    if (FAILED(hr) || !native_device) return false;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    return SUCCEEDED(native_device->CreateCommandQueue(
        &queue_desc, IID_PPV_ARGS(queue.GetAddressOf()))) &&
        queue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
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
