#include "device_context.h"
#include "guest_gpu_page_registry.h"
#include "../runtime/physical_memory.h"
#include "../runtime/xmem_bridge.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace scarlett::gpu {
HRESULT DeviceContext::guest_gpu_page_scattered_test() {
    constexpr size_t count = 8;
    constexpr size_t bytes = memory::PhysicalMemory::page_bytes;
    constexpr uint64_t four_gib = 4ull << 30;
    constexpr uint64_t eight_gib = 8ull << 30;
    constexpr uint64_t twelve_gib = 12ull << 30;
    constexpr uint64_t guest_bytes = 16ull << 30;

    struct CpuPages {
        std::array<uintptr_t, count> frames{};
        bool allocated = false;
        void* source = nullptr;
        void* mirror = nullptr;
        ~CpuPages() {
            if (source) xmem::VirtualFreeBridge(source, 0, MEM_RELEASE);
            if (mirror) xmem::VirtualFreeBridge(mirror, 0, MEM_RELEASE);
            if (allocated) xmem::XMemFreePhysicalPages(count, frames.data());
        }
    } cpu;
    uintptr_t actual = count;
    if (!xmem::XMemAllocatePhysicalPages(
            MEM_64K_PAGES, &actual, cpu.frames.data()))
        return last_error_ = HRESULT_FROM_WIN32(
            GetLastError() ? GetLastError() : ERROR_GEN_FAILURE);
    cpu.allocated = actual == count;
    if (!cpu.allocated) return last_error_ = E_FAIL;
    cpu.source = xmem::XMemVirtualAlloc(
        nullptr, count * bytes, MEM_RESERVE | MEM_64K_PAGES,
        0, PAGE_READWRITE);
    cpu.mirror = xmem::XMemVirtualAlloc(
        nullptr, count * bytes, MEM_RESERVE | MEM_64K_PAGES,
        0, PAGE_READWRITE);
    if (!cpu.source || !cpu.mirror || cpu.source == cpu.mirror)
        return last_error_ = E_OUTOFMEMORY;
    if (xmem::XMemMapPhysicalPages(
            cpu.source, count, cpu.frames.data()) != cpu.source ||
        xmem::XMemMapPhysicalPages(
            cpu.mirror, count, cpu.frames.data()) != cpu.mirror)
        return last_error_ = E_FAIL;
    auto* first = static_cast<uint8_t*>(cpu.source);
    auto* second = static_cast<uint8_t*>(cpu.mirror);

    if (!ready()) {
        last_error_ = initialize();
        if (FAILED(last_error_)) return last_error_;
    }
    Microsoft::WRL::ComPtr<ID3D12Device11> native;
    last_error_ = acquire_device11(native.GetAddressOf());
    if (FAILED(last_error_)) return last_error_;
    SparseGpuWindows windows;
    last_error_ = windows.initialize(native.Get(), guest_bytes, 2);
    if (FAILED(last_error_)) return last_error_;
    GuestGpuPageRegistry registry(
        memory::PhysicalMemory::instance(), windows, native.Get());
    using PageBinding = GuestGpuPageRegistry::PageBinding;
    using PageTransfer = GuestGpuPageRegistry::PageTransfer;
    using ScatteredPlan = GuestGpuPageRegistry::ScatteredPlan;

    // Four physical 2-page runs, with two runs straddling the 4GiB native
    // resource boundary and two straddling 8GiB. The source and alias
    // windows use different native reserved resources at all times.
    constexpr std::array<uint64_t, count> source_offsets{
        four_gib - 2*bytes, four_gib - bytes,
        four_gib, four_gib + bytes,
        eight_gib - 2*bytes, eight_gib - bytes,
        eight_gib, eight_gib + bytes
    };
    constexpr std::array<uint64_t, count> alias_offsets{
        twelve_gib - 2*bytes, twelve_gib - bytes,
        twelve_gib, twelve_gib + bytes,
        four_gib - 2*bytes, four_gib - bytes,
        four_gib, four_gib + bytes
    };
    constexpr std::array<size_t, count> shuffled_ids{6,2,0,5,7,1,4,3};
    std::array<PageBinding, count> ordered{};
    std::array<PageBinding, count> shuffled{};
    std::array<uintptr_t, count> shuffled_frames{};
    for (size_t i = 0; i < count; ++i) {
        ordered[i] = {cpu.frames[i], first + i*bytes, second + i*bytes,
                      {0, source_offsets[i]}, {1, alias_offsets[i]}};
    }
    for (size_t i = 0; i < count; ++i) {
        shuffled[i] = ordered[shuffled_ids[i]];
        shuffled_frames[i] = shuffled[i].frame;
    }

    ScatteredPlan plan;
    last_error_ = registry.plan_scattered(shuffled, plan);
    if (FAILED(last_error_) || plan.runs.size() != 4 ||
        plan.mapping_calls() != 8 ||
        registry.pinned_alias_count() != 0 ||
        windows.mapped_tile_count() != 0)
        return last_error_ = E_FAIL;
    for (const auto& run : plan.runs)
        if (run.size() != 2) return last_error_ = E_FAIL;

    // Invalid requests have NO native mapping calls and NO CPU pins.
    auto wrong = shuffled;
    wrong[4].frame = wrong[0].frame;
    if (registry.plan_scattered(wrong, plan) != E_INVALIDARG)
        return last_error_ = E_FAIL;
    wrong = shuffled;
    wrong[4].alias = wrong[0].source;
    if (registry.plan_scattered(wrong, plan) != E_INVALIDARG)
        return last_error_ = E_FAIL;
    wrong = shuffled;
    wrong[4].source.offset += 1;
    if (registry.plan_scattered(wrong, plan) != E_INVALIDARG)
        return last_error_ = E_FAIL;
    wrong = shuffled;
    wrong[4].alias.window = 99;
    if (registry.plan_scattered(wrong, plan) != E_INVALIDARG)
        return last_error_ = E_FAIL;
    wrong = shuffled;
    wrong[4].second_cpu_alias = second + (shuffled_ids[0] * bytes);
    if (registry.bind_scattered(wrong) !=
            HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS) ||
        registry.pinned_alias_count() != 0 ||
        registry.native_mapping_count() != 0 ||
        registry.binding_count() != 0)
        return last_error_ = E_FAIL;

    last_error_ = registry.bind_scattered(shuffled);
    if (FAILED(last_error_) ||
        registry.native_mapping_count() != 8 ||
        registry.binding_count() != count ||
        registry.used_heap_tile_count() != count ||
        registry.pinned_alias_count() != 2*count ||
        windows.mapped_tile_count() != 2*count ||
        registry.native_queue_count() != 1 ||
        registry.physical_heap_count() != 1)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;

    std::array<bool, GuestGpuPageRegistry::heap_capacity_tiles> seen_tiles{};
    for (size_t i = 0; i < count; ++i) {
        const auto heap_tile = registry.heap_tile_for(cpu.frames[i]);
        if (!heap_tile || *heap_tile >= seen_tiles.size() ||
            seen_tiles[*heap_tile]) return last_error_ = E_FAIL;
        seen_tiles[*heap_tile] = true;
    }
    if (registry.bind_scattered(shuffled) !=
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) ||
        registry.pinned_alias_count() != 2*count ||
        registry.native_mapping_count() != 8)
        return last_error_ = E_FAIL;

    std::vector<uint8_t> original(count*bytes), modified(count*bytes);
    std::vector<uint8_t> returned(count*bytes, 0xDB);
    for (size_t i = 0; i < count*bytes; ++i) {
        const size_t frame = i / bytes;
        const size_t byte = i % bytes;
        original[i] = static_cast<uint8_t>(
            (byte*37 + (byte>>6)*19 + frame*59 + 0x45) & 255);
        modified[i] = static_cast<uint8_t>(
            (byte*71 + (byte>>7)*13 + frame*31 + 0xA7) & 255);
    }
    std::memcpy(first, original.data(), original.size());
    if (std::memcmp(second, original.data(), original.size()) != 0)
        return last_error_ = HRESULT_FROM_WIN32(ERROR_CRC);

    std::array<PageTransfer, count> work{};
    for (size_t i = 0; i < count; ++i) {
        work[i] = {cpu.frames[i],
                   std::span<const uint8_t>(original.data()+i*bytes, bytes),
                   std::span<uint8_t>(returned.data()+i*bytes, bytes),
                   false};
    }
    last_error_ = registry.transfer_batch(work);
    if (FAILED(last_error_) ||
        registry.submitted_transfers() != count ||
        registry.queue_submission_count() != 1 ||
        std::memcmp(returned.data(), original.data(), original.size()) != 0)
        return last_error_ = FAILED(last_error_) ? last_error_ :
            HRESULT_FROM_WIN32(ERROR_CRC);
    for (size_t i = 0; i < count; ++i) {
        work[i].input = std::span<const uint8_t>(
            modified.data()+i*bytes, bytes);
        work[i].writeback_cpu = true;
    }
    last_error_ = registry.transfer_batch(work);
    if (FAILED(last_error_) ||
        registry.submitted_transfers() != count*2 ||
        registry.queue_submission_count() != 2 ||
        std::memcmp(returned.data(), modified.data(), modified.size()) != 0 ||
        std::memcmp(first, modified.data(), modified.size()) != 0 ||
        std::memcmp(second, modified.data(), modified.size()) != 0)
        return last_error_ = FAILED(last_error_) ? last_error_ :
            HRESULT_FROM_WIN32(ERROR_CRC);

    const uint64_t initial_unmaps = registry.native_unmapping_count();
    last_error_ = registry.unbind_scattered(shuffled_frames);
    if (FAILED(last_error_) ||
        registry.binding_count() != 0 ||
        registry.pinned_alias_count() != 0 ||
        windows.mapped_tile_count() != 0 ||
        registry.used_heap_tile_count() != 0 ||
        registry.native_unmapping_count() != initial_unmaps + 8)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;

    // Inject a synthetic mapping-completion timeout in the THIRD run.
    // The first two runs were already published, so rollback must retire
    // all three (including the failed run) and unpin all outer CPU aliases.
    registry.inject_scattered_run_failure_for_test(2);
    constexpr HRESULT timeout = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    const uint64_t maps_before_failure = registry.native_mapping_count();
    const uint64_t unmaps_before_failure = registry.native_unmapping_count();
    if (registry.bind_scattered(shuffled) != timeout ||
        registry.native_mapping_count() != maps_before_failure + 6 ||
        registry.native_unmapping_count() != unmaps_before_failure + 6 ||
        registry.binding_count() != 0 ||
        windows.mapped_tile_count() != 0 ||
        registry.pinned_alias_count() != 0 ||
        registry.used_heap_tile_count() != 0 ||
        std::memcmp(first, modified.data(), modified.size()) != 0)
        return last_error_ = E_FAIL;

    // Cleanly rebind, verify real GPU readback from all eight virtual
    // source/alias pairs and leave the CPU XMem views unmodified.
    last_error_ = registry.bind_scattered(shuffled);
    if (FAILED(last_error_) ||
        registry.native_mapping_count() != 22 ||
        registry.binding_count() != count ||
        registry.pinned_alias_count() != count*2)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
    for (size_t i = 0; i < count; ++i) {
        work[i].input = std::span<const uint8_t>(
            original.data()+i*bytes, bytes);
        work[i].writeback_cpu = false;
    }
    last_error_ = registry.transfer_batch(work);
    if (FAILED(last_error_) ||
        registry.submitted_transfers() != count*3 ||
        registry.queue_submission_count() != 3 ||
        std::memcmp(returned.data(), original.data(), original.size()) != 0 ||
        std::memcmp(first, modified.data(), modified.size()) != 0 ||
        std::memcmp(second, modified.data(), modified.size()) != 0)
        return last_error_ = FAILED(last_error_) ? last_error_ :
            HRESULT_FROM_WIN32(ERROR_CRC);

    // Interrupted native NULL unmap in the first sorted two-page run
    // must leave the other 6 frames intact. Retrying the same frame
    // list completes the missing alias NULL update without double-unmap.
    registry.inject_next_unmap_failure_after_source_for_test();
    const uint64_t before_partial = registry.native_unmapping_count();
    if (registry.unbind_scattered(shuffled_frames) != timeout ||
        registry.native_unmapping_count() != before_partial + 1 ||
        registry.binding_count() != count ||
        registry.pinned_alias_count() != count*2 ||
        windows.mapped_tile_count() != count*2 - 2 ||
        xmem::XMemMapPhysicalPages(first, 1, nullptr) != nullptr ||
        GetLastError() != ERROR_BUSY)
        return last_error_ = E_FAIL;
    last_error_ = registry.unbind_scattered(shuffled_frames);
    if (FAILED(last_error_) ||
        registry.native_unmapping_count() != before_partial + 8 ||
        registry.binding_count() != 0 ||
        registry.pinned_alias_count() != 0 ||
        registry.used_heap_tile_count() != 0 ||
        windows.mapped_tile_count() != 0)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;

    scattered_run_count_ = 4;
    registry_batch_mapping_updates_ = registry.native_mapping_count();
    registry_grouped_unmapping_updates_ = registry.native_unmapping_count();
    registry_queue_submissions_ = registry.queue_submission_count();
    submitted_ = registry.submitted_transfers();
    last_error_ = registry.clear();
    if (FAILED(last_error_) ||
        registry.binding_count() != 0 ||
        registry.pinned_alias_count() != 0 ||
        registry.physical_heap_count() != 0)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
    windows.clear();
    return last_error_ = S_OK;
}
} // namespace scarlett::gpu
