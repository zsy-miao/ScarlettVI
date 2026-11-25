#include "device_context.h"
#include "guest_gpu_page_registry.h"
#include "../runtime/physical_memory.h"
#include "../runtime/xmem_bridge.h"
#include <array>
#include <cstring>
#include <vector>

namespace scarlett::gpu {
HRESULT DeviceContext::guest_gpu_page_batch_test() {
    constexpr size_t count = 4;
    constexpr size_t bytes = memory::PhysicalMemory::page_bytes;
    constexpr uint64_t guest_bytes = 16ull << 30;
    constexpr uint64_t source_base = (4ull << 30) + 5 * bytes;
    constexpr uint64_t alias_base = (12ull << 30) + 9 * bytes;

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
    std::array<PageBinding, count> requests{};
    for (size_t i = 0; i < count; ++i) {
        requests[i] = {
            cpu.frames[i], first + i * bytes, second + i * bytes,
            {0, source_base + i * bytes}, {1, alias_base + i * bytes}
        };
    }
    // Batch preflight errors must reject BEFORE pinning CPU aliases or
    // enqueuing native GPU UpdateTileMappings. These use the public API.
    auto invalid = requests;
    invalid[2].frame = invalid[0].frame;
    if (registry.bind_batch(invalid) != E_INVALIDARG)
        return last_error_ = E_FAIL;
    invalid = requests;
    invalid[2].second_cpu_alias = first + 2 * bytes;
    if (registry.bind_batch(invalid) != E_INVALIDARG)
        return last_error_ = E_FAIL;
    invalid = requests;
    invalid[2].second_cpu_alias = second + 3 * bytes; // wrong physical frame
    if (registry.bind_batch(invalid) !=
            HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS) ||
        registry.pinned_alias_count() != 0 ||
        registry.native_mapping_count() != 0)
        return last_error_ = E_FAIL;
    invalid = requests;
    invalid[1].source.offset += bytes;
    if (registry.bind_batch(invalid) != E_INVALIDARG)
        return last_error_ = E_FAIL;
    invalid = requests;
    // Still contiguous as a guest region, but it crosses the actual
    // 8 GiB boundary between two native 4 GiB D3D12 resources.
    for (size_t i = 0; i < count; ++i)
        invalid[i].source.offset = (8ull << 30) - 2 * bytes + i * bytes;
    if (registry.bind_batch(invalid) != E_INVALIDARG)
        return last_error_ = E_FAIL;
    if (registry.bind_batch({}) != E_INVALIDARG ||
        registry.pinned_alias_count() != 0 ||
        registry.binding_count() != 0 ||
        registry.native_mapping_count() != 0 ||
        windows.mapped_tile_count() != 0)
        return last_error_ = E_FAIL;

    last_error_ = registry.bind_batch(requests);
    if (FAILED(last_error_)) return last_error_;
    if (registry.bind_batch(requests) !=
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) ||
        registry.native_mapping_count() != 2 ||
        windows.mapped_tile_count() != 8 ||
        registry.binding_count() != count ||
        registry.used_heap_tile_count() != count ||
        registry.native_queue_count() != 1 ||
        registry.physical_heap_count() != 1 ||
        registry.pinned_alias_count() != 2 * count)
        return last_error_ = E_FAIL;
    for (size_t i = 0; i < count; ++i)
        if (registry.heap_tile_for(cpu.frames[i]) != i)
            return last_error_ = E_FAIL;

    // Grouped unmap preflight rejects duplicate/unordered frame lists and
    // a failed pre-unmap GPU wait WITHOUT changing either GPU alias group.
    const std::array<uintptr_t, count> ordered_frames = cpu.frames;
    auto invalid_unbind = ordered_frames;
    invalid_unbind[1] = invalid_unbind[0];
    if (registry.unbind_batch({}) != E_INVALIDARG ||
        registry.unbind_batch(invalid_unbind) != E_INVALIDARG)
        return last_error_ = E_FAIL;
    invalid_unbind = ordered_frames;
    std::swap(invalid_unbind[1], invalid_unbind[2]);
    if (registry.unbind_batch(invalid_unbind) != E_INVALIDARG ||
        !registry.inject_next_gpu_wait_failure_for_test(cpu.frames[2]) ||
        registry.unbind_batch(ordered_frames) !=
            HRESULT_FROM_WIN32(ERROR_TIMEOUT) ||
        registry.native_unmapping_count() != 0 ||
        windows.mapped_tile_count() != 2 * count ||
        registry.pinned_alias_count() != 2 * count ||
        registry.binding_count() != count)
        return last_error_ = E_FAIL;

    void* const to_unmap[]{first, first + bytes};
    if (xmem::XMemMapPhysicalPagesScatter(
            to_unmap, 2, nullptr) != FALSE ||
        GetLastError() != ERROR_BUSY ||
        xmem::VirtualFreeBridge(cpu.source, 0, MEM_RELEASE) != FALSE ||
        GetLastError() != ERROR_BUSY)
        return last_error_ = E_FAIL;

    std::vector<uint8_t> pattern(count * bytes), next_pattern(count * bytes);
    std::vector<uint8_t> gpu_output(count * bytes, 0xEE);
    std::array<PageTransfer, count> transfers{};
    for (size_t i = 0; i < pattern.size(); ++i) {
        const size_t page = i / bytes;
        const size_t offset = i % bytes;
        pattern[i] = static_cast<uint8_t>(
            (offset * 17 + (offset >> 5) * 13 + page * 47 + 0x41) & 255);
        next_pattern[i] = static_cast<uint8_t>(
            (offset * 53 + (offset >> 7) * 29 + page * 67 + 0xA3) & 255);
    }
    std::memcpy(first, pattern.data(), pattern.size());
    if (std::memcmp(second, pattern.data(), pattern.size()) != 0)
        return last_error_ = HRESULT_FROM_WIN32(ERROR_CRC);

    for (size_t i = 0; i < count; ++i)
        transfers[i] = {
            cpu.frames[i],
            std::span<const uint8_t>(pattern.data() + i * bytes, bytes),
            std::span<uint8_t>(gpu_output.data() + i * bytes, bytes),
            false
        };
    last_error_ = registry.transfer_batch(transfers);
    if (FAILED(last_error_) ||
        registry.submitted_transfers() != count ||
        registry.queue_submission_count() != 1 ||
        std::memcmp(gpu_output.data(), pattern.data(), pattern.size()) != 0 ||
        std::memcmp(second, pattern.data(), pattern.size()) != 0)
        return last_error_ = FAILED(last_error_) ? last_error_ :
            HRESULT_FROM_WIN32(ERROR_CRC);

    // The same four stagers can record another four command lists and
    // explicitly copy the completed readback to pinned XMem CPU aliases.
    for (size_t i = 0; i < count; ++i) {
        transfers[i].input = std::span<const uint8_t>(
            next_pattern.data() + i * bytes, bytes);
        transfers[i].writeback_cpu = true;
    }
    last_error_ = registry.transfer_batch(transfers);
    if (FAILED(last_error_) ||
        registry.submitted_transfers() != 2 * count ||
        registry.queue_submission_count() != 2 ||
        std::memcmp(gpu_output.data(), next_pattern.data(),
                    next_pattern.size()) != 0 ||
        std::memcmp(second, next_pattern.data(),
                    next_pattern.size()) != 0 ||
        std::memcmp(first, next_pattern.data(),
                    next_pattern.size()) != 0)
        return last_error_ = FAILED(last_error_) ? last_error_ :
            HRESULT_FROM_WIN32(ERROR_CRC);

    // Remove one physical frame and rebind that same freed heap slot via
    // the original single-frame path, proving both APIs can coexist.
    last_error_ = registry.unbind(cpu.frames[0]);
    if (FAILED(last_error_)) return last_error_;
    if (registry.heap_tile_for(cpu.frames[0]).has_value() ||
        registry.pinned_alias_count() != 2 * (count - 1) ||
        windows.mapped_tile_count() != 2 * (count - 1))
        return last_error_ = E_FAIL;
    last_error_ = registry.bind(
        cpu.frames[0], first, second,
        requests[0].source, requests[0].alias);
    if (FAILED(last_error_) ||
        registry.heap_tile_for(cpu.frames[0]) != 0u ||
        windows.mapped_tile_count() != 8 ||
        registry.native_mapping_count() != 4)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;

    last_error_ = registry.transfer_batch(transfers);
    if (FAILED(last_error_) ||
        registry.submitted_transfers() != 3 * count ||
        registry.queue_submission_count() != 3 ||
        std::memcmp(gpu_output.data(), next_pattern.data(),
                    next_pattern.size()) != 0)
        return last_error_ = FAILED(last_error_) ? last_error_ :
            HRESULT_FROM_WIN32(ERROR_CRC);

    // Verify fail-closed mapping error after REAL two-resource tile enqueue,
    // without physical device removal. Reopen a fresh four-frame batch,
    // intentionally interrupt its mapping fence and ensure NONE of the
    // eight CPU aliases or physical tile slots can be prematurely released.
    last_error_ = registry.clear();
    if (FAILED(last_error_) ||
        registry.pinned_alias_count() != 0 ||
        windows.mapped_tile_count() != 0 ||
        registry.physical_heap_count() != 0)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;

    registry.inject_next_mapping_wait_failure_for_test();
    const uint64_t before_failed_mapping = registry.native_mapping_count();
    if (registry.bind_batch(requests) !=
            HRESULT_FROM_WIN32(ERROR_TIMEOUT) ||
        registry.native_mapping_count() != before_failed_mapping + 2 ||
        registry.binding_count() != count ||
        registry.pinned_alias_count() != count * 2 ||
        windows.mapped_tile_count() != count * 2 ||
        registry.used_heap_tile_count() != count ||
        registry.transfer_batch(transfers) != E_UNEXPECTED ||
        registry.queue_submission_count() != 3 ||
        xmem::XMemMapPhysicalPages(first, 1, nullptr) != nullptr ||
        GetLastError() != ERROR_BUSY)
        return last_error_ = E_FAIL;

    last_error_ = registry.clear();
    if (FAILED(last_error_) ||
        registry.pinned_alias_count() != 0 ||
        registry.binding_count() != 0 ||
        registry.used_heap_tile_count() != 0 ||
        registry.physical_heap_count() != 0 ||
        windows.mapped_tile_count() != 0)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;

    // Prove the entire cohort is usable again after an injected mapping
    // wait failure was retired. A final one-call-per-alias registration and
    // real four-page GPU copy must still work with the same physical IDs.
    last_error_ = registry.bind_batch(requests);
    if (FAILED(last_error_) ||
        registry.native_mapping_count() != 8 ||
        registry.pinned_alias_count() != count * 2)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
    last_error_ = registry.transfer_batch(transfers);
    if (FAILED(last_error_) ||
        registry.submitted_transfers() != 4 * count ||
        registry.queue_submission_count() != 4 ||
        std::memcmp(gpu_output.data(), next_pattern.data(),
                    next_pattern.size()) != 0)
        return last_error_ = FAILED(last_error_) ? last_error_ :
            HRESULT_FROM_WIN32(ERROR_CRC);
    // Verify grouped UNMAP, including a retry after only the first
    // real native NULL mapping was queued. Every CPU alias and physical
    // GPU heap slot must remain reserved until the second group's NULL
    // mapping and the final completion fence have succeeded.
    constexpr HRESULT timeout = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    const std::array<uintptr_t, count> frames = cpu.frames;
    auto invalid_frames = frames;
    invalid_frames[1] = invalid_frames[0];
    if (registry.unbind_batch(invalid_frames) != E_INVALIDARG ||
        registry.unbind_batch({}) != E_INVALIDARG)
        return last_error_ = E_FAIL;

    const uint64_t unmap_before = registry.native_unmapping_count();
    registry.inject_next_unmap_failure_after_source_for_test();
    if (registry.unbind_batch(frames) != timeout ||
        registry.native_unmapping_count() != unmap_before + 1 ||
        registry.binding_count() != count ||
        registry.used_heap_tile_count() != count ||
        registry.pinned_alias_count() != count * 2 ||
        windows.mapped_tile_count() != count ||
        registry.transfer_batch(transfers) != E_UNEXPECTED ||
        xmem::XMemMapPhysicalPages(first, 1, nullptr) != nullptr ||
        GetLastError() != ERROR_BUSY)
        return last_error_ = E_FAIL;

    // A second attempt must skip the already-queued source NULL mapping,
    // enqueue just the other GROUP and fence it before freeing pins.
    last_error_ = registry.unbind_batch(frames);
    if (FAILED(last_error_) ||
        registry.native_unmapping_count() != unmap_before + 2 ||
        registry.binding_count() != 0 ||
        registry.pinned_alias_count() != 0 ||
        registry.used_heap_tile_count() != 0 ||
        windows.mapped_tile_count() != 0 ||
        registry.physical_heap_count() != 1)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
    registry_grouped_unmapping_updates_ +=
        registry.native_unmapping_count() - unmap_before;

    // The old physical frames/CPU alias VAs and heap slots are reusable.
    // A new four-page GPU registration/copy demonstrates safe reuse after
    // two actual native NULL updates instead of eight independent ones.
    last_error_ = registry.bind_batch(requests);
    if (FAILED(last_error_) ||
        registry.native_mapping_count() != 10 ||
        registry.binding_count() != count)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
    last_error_ = registry.transfer_batch(transfers);
    if (FAILED(last_error_) ||
        registry.submitted_transfers() != 5 * count ||
        registry.queue_submission_count() != 5 ||
        std::memcmp(gpu_output.data(), next_pattern.data(),
                    next_pattern.size()) != 0)
        return last_error_ = FAILED(last_error_) ? last_error_ :
            HRESULT_FROM_WIN32(ERROR_CRC);

    // A different injected failure happens AFTER both actual NULL mapping
    // calls, before the post-unmap queue fence. Existing clear() must fence
    // pending work and release every pinned CPU frame without double unmap.
    const uint64_t second_unmap_before = registry.native_unmapping_count();
    registry.inject_next_unmap_failure_after_both_for_test();
    if (registry.unbind_batch(frames) != timeout ||
        registry.native_unmapping_count() != second_unmap_before + 2 ||
        registry.binding_count() != count ||
        registry.pinned_alias_count() != count * 2 ||
        registry.used_heap_tile_count() != count ||
        windows.mapped_tile_count() != 0 ||
        xmem::XMemMapPhysicalPages(first, 1, nullptr) != nullptr ||
        GetLastError() != ERROR_BUSY)
        return last_error_ = E_FAIL;
    registry_grouped_unmapping_updates_ +=
        registry.native_unmapping_count() - second_unmap_before;

    last_error_ = registry.clear();
    if (FAILED(last_error_) ||
        registry.pinned_alias_count() != 0 ||
        registry.physical_heap_count() != 0 ||
        registry.used_heap_tile_count() != 0 ||
        registry.binding_count() != 0 ||
        windows.mapped_tile_count() != 0 ||
        registry_grouped_unmapping_updates_ != 4)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;

    registry_batch_mapping_updates_ = registry.native_mapping_count();
    registry_queue_submissions_ = registry.queue_submission_count();
    submitted_ = registry.submitted_transfers();
    windows.clear();
    return last_error_ = S_OK;
}
} // namespace scarlett::gpu
