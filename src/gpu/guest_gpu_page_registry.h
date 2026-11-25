#pragma once
#include "gpu_page_stager.h"
#include "../runtime/physical_memory.h"
#include <cstddef>
#include <compare>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>

namespace scarlett::gpu {

// Host-only registry tying original XMem 64KiB frame IDs and *two verified
// CPU aliases* to one staged physical D3D12 tile. Registrations pin both
// CPU aliases: VirtualFree/unmap must fail with ERROR_BUSY until unbound.
// No Xbox GPU physical page-table ABI or unified CPU/GPU memory is implied.
class GuestGpuPageRegistry final {
public:
    struct GpuSlot {
        size_t window = 0;
        uint64_t offset = 0;
        bool operator==(const GpuSlot&) const = default;
        auto operator<=>(const GpuSlot&) const = default;
    };
    GuestGpuPageRegistry(memory::PhysicalMemory& physical,
                         SparseGpuWindows& windows,
                         ID3D12Device11* device) noexcept;
    ~GuestGpuPageRegistry();
    GuestGpuPageRegistry(const GuestGpuPageRegistry&) = delete;
    GuestGpuPageRegistry& operator=(const GuestGpuPageRegistry&) = delete;

    // Only validated, already mapped XMem alias VAs are accepted.
    // Each frame and each GPU slot may be registered only once.
    HRESULT bind(uintptr_t frame, void* first_cpu_alias, void* second_cpu_alias,
                 GpuSlot source_gpu, GpuSlot alias_gpu) noexcept;

    // Uses a real D3D12 queue and fence. Source may be the first CPU alias
    // or independent generated host bytes. Output must be a disjoint 64KiB
    // scratch span (not either registered CPU alias). On writeback, the
    // *completed* GPU result is copied to the second pinned CPU alias.
    HRESULT transfer(uintptr_t frame, std::span<const uint8_t> input,
                     std::span<uint8_t> output, bool writeback_cpu) noexcept;
    HRESULT unbind(uintptr_t frame) noexcept;
    HRESULT clear() noexcept;
    size_t binding_count() const noexcept;
    uint64_t submitted_transfers() const noexcept;
    size_t pinned_alias_count() const;

private:
    struct Binding {
        uint8_t* first_cpu = nullptr;
        uint8_t* second_cpu = nullptr;
        GpuSlot source{};
        GpuSlot alias{};
        std::unique_ptr<GpuPageStager> stager;
    };
    bool overlaps_cpu(const Binding& entry, std::span<uint8_t> output) const noexcept;

    memory::PhysicalMemory& physical_;
    SparseGpuWindows& windows_;
    Microsoft::WRL::ComPtr<ID3D12Device11> device_;
    mutable std::mutex mutex_;
    std::unordered_map<uintptr_t, Binding> bindings_;
    std::map<GpuSlot, uintptr_t> gpu_slots_;
    uint64_t completed_transfers_ = 0;
};
} // namespace scarlett::gpu
