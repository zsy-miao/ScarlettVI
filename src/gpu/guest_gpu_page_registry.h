#pragma once
#include "gpu_page_stager.h"
#include "../runtime/physical_memory.h"
#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>
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
    struct PageBinding {
        uintptr_t frame = 0;
        void* first_cpu_alias = nullptr;
        void* second_cpu_alias = nullptr;
        GpuSlot source{};
        GpuSlot alias{};
    };
    // All slots in each alias group must be adjacent within ONE native
    // 4GiB reserved GPU resource. One UpdateTileMappings per alias group;
    // all CPU pins are retained until an acknowledged GPU completion fence.
    HRESULT bind_batch(std::span<const PageBinding> pages) noexcept;
    // Batch NULL-maps two contiguous GPU alias groups, with a fence before
    // and after mapping updates. Failed waits retain CPU pins/heap slots;
    // retries complete only the pending alias group(s).
    HRESULT unbind_batch(std::span<const uintptr_t> frames) noexcept;
    // Host-only scattered planner: groups unordered pages by contiguous
    // source AND alias tiles in matching native D3D12 reserved resources.
    // The planner has no GPU/CPU mapping side effects; binding pins ALL
    // requested CPU aliases before submitting the first native GPU update.
    struct ScatteredPlan {
        std::vector<std::vector<PageBinding>> runs;
        size_t mapping_calls() const noexcept { return runs.size() * 2; }
    };
    HRESULT plan_scattered(std::span<const PageBinding> pages,
                           ScatteredPlan& output) const noexcept;
    HRESULT bind_scattered(std::span<const PageBinding> pages) noexcept;
    // Retires the supplied still-live scattered frame IDs by planned runs.
    // A failed cohort stays pinned; previously completed cohorts stay retired.
    HRESULT unbind_scattered(std::span<const uintptr_t> frames) noexcept;

    // Uses a real D3D12 queue and fence. Source may be the first CPU alias
    // or independent generated host bytes. Output must be a disjoint 64KiB
    // scratch span (not either registered CPU alias). On writeback, the
    // *completed* GPU result is copied to the second pinned CPU alias.
    HRESULT transfer(uintptr_t frame, std::span<const uint8_t> input,
                     std::span<uint8_t> output, bool writeback_cpu) noexcept;
    struct PageTransfer {
        uintptr_t frame = 0;
        std::span<const uint8_t> input;
        std::span<uint8_t> output;
        bool writeback_cpu = false;
    };
    // Validates the whole request, then submits N independent page command
    // lists in ONE real ExecuteCommandLists call and waits on ONE batch fence.
    HRESULT transfer_batch(std::span<const PageTransfer> requests) noexcept;
    uint64_t queue_submission_count() const noexcept;
    size_t native_queue_count() const noexcept;
    // Actual native host UpdateTileMappings calls across both bind paths.
    uint64_t native_mapping_count() const noexcept;
    uint64_t native_unmapping_count() const noexcept;
    // One 4MiB default heap with 64 separately assignable 64KiB host tiles.
    static constexpr uint32_t heap_capacity_tiles = 64;
    size_t physical_heap_count() const noexcept;
    size_t used_heap_tile_count() const noexcept;
    std::optional<uint32_t> heap_tile_for(uintptr_t frame) const noexcept;
    HRESULT unbind(uintptr_t frame) noexcept;
    // On fence failure, retains still-active frame bindings and CPU pins
    // so callers can safely retry rather than retiring live GPU memory.
    HRESULT clear() noexcept;
    size_t binding_count() const noexcept;
    uint64_t submitted_transfers() const noexcept;
    size_t pinned_alias_count() const;

private:
    friend class DeviceContext; // fixture-only failure injection
    bool inject_next_gpu_wait_failure_for_test(
        uintptr_t frame, uint32_t successful_waits_before_failure = 0) noexcept;
    void inject_next_batch_wait_failure_for_test() noexcept;
    void inject_next_mapping_wait_failure_for_test() noexcept;
    void inject_next_unmap_failure_after_source_for_test() noexcept;
    void inject_next_unmap_failure_after_both_for_test() noexcept;
    void inject_scattered_run_failure_for_test(size_t run_index) noexcept;
    struct Binding {
        uint8_t* first_cpu = nullptr;
        uint8_t* second_cpu = nullptr;
        GpuSlot source{};
        GpuSlot alias{};
        uint32_t heap_tile = 0;
        std::unique_ptr<GpuPageStager> stager;
    };
    bool overlaps_cpu(const Binding& entry, std::span<uint8_t> output) const noexcept;

    memory::PhysicalMemory& physical_;
    SparseGpuWindows& windows_;
    Microsoft::WRL::ComPtr<ID3D12Device11> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Heap> shared_tile_heap_;
    std::array<bool, heap_capacity_tiles> occupied_heap_tiles_{};
    Microsoft::WRL::ComPtr<ID3D12Fence> batch_fence_;
    HANDLE batch_event_ = nullptr;
    uint64_t next_batch_fence_ = 1;
    uint64_t gpu_submissions_ = 0;
    bool test_fail_next_batch_wait_ = false;
    bool test_fail_next_mapping_wait_ = false;
    bool test_fail_next_unmap_after_source_ = false;
    bool test_fail_next_unmap_after_both_ = false;
    std::optional<size_t> test_fail_scattered_run_;
    mutable std::mutex mutex_;
    std::unordered_map<uintptr_t, Binding> bindings_;
    std::map<GpuSlot, uintptr_t> gpu_slots_;
    uint64_t completed_transfers_ = 0;
};
} // namespace scarlett::gpu
