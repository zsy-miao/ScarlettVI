#include "sparse_gpu_windows.h"
#include <limits>

namespace scarlett::gpu {
SparseGpuWindows::~SparseGpuWindows() { clear(); }

void SparseGpuWindows::clear() noexcept {
    for (const auto& window : windows_)
        addresses_.unregister_buffer(window.synthetic_guest_base);
    windows_.clear();
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
    // The host must be able to represent each requested resource AND all
    // simultaneously reserved ranges. Include space for driver bookkeeping.
    if (gpu_va_bits_ >= 63 || resource_bits_ >= 63 ||
        gpu_va_bits_ < 36 || resource_bits_ < 36 ||
        (uint64_t{1} << resource_bits_) < bytes ||
        (uint64_t{1} << gpu_va_bits_) < bytes * (count + 1ull))
        return DXGI_ERROR_UNSUPPORTED;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    constexpr uint64_t kGuestStart = 0x400000000000ull;
    constexpr uint64_t kGuestStride = 64ull * 1024 * 1024 * 1024;
    try {
        windows_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Window window{};
            window.synthetic_guest_base = kGuestStart + i * kGuestStride;
            window.bytes = bytes;
            hr = device->CreateReservedResource(
                &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(window.reserved.GetAddressOf()));
            if (FAILED(hr)) {
                clear();
                return hr;
            }
            // The resource exists in the real Windows GPU VA space, but
            // cannot be used for shader reads until it is tile-mapped.
            windows_.push_back(std::move(window));
            if (!addresses_.register_buffer(windows_.back().synthetic_guest_base,
                                            bytes, windows_.back().reserved.Get())) {
                clear();
                return E_FAIL;
            }
        }
    } catch (...) {
        clear();
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

bool SparseGpuWindows::verify_boundaries() const noexcept {
    if (windows_.empty() || addresses_.resource_count() != windows_.size())
        return false;
    for (const auto& window : windows_) {
        const auto first = addresses_.resolve(
            window.synthetic_guest_base, GuestAddressSpace::guest_alignment);
        const auto last = addresses_.resolve(
            window.synthetic_guest_base + window.bytes -
            GuestAddressSpace::guest_alignment, GuestAddressSpace::guest_alignment);
        const auto outside = addresses_.resolve(
            window.synthetic_guest_base + window.bytes - 4, 8);
        if (!first || !last || outside ||
            first->resource.Get() != window.reserved.Get() ||
            last->resource.Get() != window.reserved.Get() ||
            last->host_gpu_va != first->host_gpu_va + window.bytes -
                                  GuestAddressSpace::guest_alignment)
            return false;
    }
    return true;
}
} // namespace scarlett::gpu
