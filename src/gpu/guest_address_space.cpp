#include "guest_address_space.h"
#include <limits>
#include <utility>

namespace scarlett::gpu {
bool GuestAddressSpace::register_buffer(uint64_t guest_start, uint64_t byte_count,
                                        ID3D12Resource* resource) {
    if (!resource || !byte_count || (guest_start % guest_alignment) != 0 ||
        (byte_count % guest_alignment) != 0 ||
        guest_start > std::numeric_limits<uint64_t>::max() - byte_count)
        return false;

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const uint64_t host_gpu_va = resource->GetGPUVirtualAddress();
    // D3D12 buffers have an address; textures don't. Console tiled surfaces
    // need separate layout-aware translation and are not accepted here.
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        desc.Width < byte_count || host_gpu_va == 0 ||
        host_gpu_va > std::numeric_limits<uint64_t>::max() - byte_count)
        return false;
    std::lock_guard lock(mutex_);
    const uint64_t end = guest_start + byte_count;
    auto it = entries_.lower_bound(guest_start);
    if (it != entries_.end() && it->first < end) return false;
    if (it != entries_.begin()) {
        const auto& before = *std::prev(it);
        if (before.second.length_bytes > guest_start - before.first)
            return false;
    }
    Entry entry;
    entry.length_bytes = byte_count;
    entry.resource = resource;
    entry.host_gpu_va = host_gpu_va;
    return entries_.emplace(guest_start, std::move(entry)).second;
}

std::optional<ResolvedGpuBuffer> GuestAddressSpace::resolve(
    uint64_t address, uint64_t byte_count) const {
    if (!byte_count) return std::nullopt;
    std::lock_guard lock(mutex_);
    auto it = entries_.upper_bound(address);
    if (it == entries_.begin()) return std::nullopt;
    --it;
    if (address < it->first) return std::nullopt;
    const uint64_t offset = address - it->first;
    if (offset >= it->second.length_bytes ||
        byte_count > it->second.length_bytes - offset)
        return std::nullopt;
    ResolvedGpuBuffer translated;
    translated.resource = it->second.resource;
    translated.host_gpu_va = it->second.host_gpu_va + offset;
    translated.offset_bytes = offset;
    translated.length_bytes = byte_count;
    return translated;
}

bool GuestAddressSpace::unregister_buffer(uint64_t guest_start) {
    std::lock_guard lock(mutex_);
    return entries_.erase(guest_start) == 1;
}

size_t GuestAddressSpace::resource_count() const noexcept {
    std::lock_guard lock(mutex_);
    return entries_.size();
}
} // namespace scarlett::gpu
