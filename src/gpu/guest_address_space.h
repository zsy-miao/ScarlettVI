#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

namespace scarlett::gpu {

// An independent console-VA -> host-D3D12-buffer translation table.
// Guest addresses are SYNTHETIC: they are not Xbox GPU page table entries.
// Only linear host buffers are supported. D3D12 resources remain alive for
// the lifetime of each translation entry and active resolved snapshot.
struct ResolvedGpuBuffer {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint64_t host_gpu_va = 0;
    uint64_t offset_bytes = 0;
    uint64_t length_bytes = 0;
};

class GuestAddressSpace final {
public:
    static constexpr uint64_t guest_alignment = 64 * 1024;
    GuestAddressSpace() = default;
    GuestAddressSpace(const GuestAddressSpace&) = delete;
    GuestAddressSpace& operator=(const GuestAddressSpace&) = delete;

    bool register_buffer(uint64_t guest_start, uint64_t byte_count, ID3D12Resource* resource);
    std::optional<ResolvedGpuBuffer> resolve(uint64_t guest_address, uint64_t byte_count) const;
    bool unregister_buffer(uint64_t guest_start);
    size_t resource_count() const noexcept;

private:
    struct Entry {
        uint64_t length_bytes = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint64_t host_gpu_va = 0;
    };
    mutable std::mutex mutex_;
    std::map<uint64_t, Entry> entries_;
};
} // namespace scarlett::gpu
