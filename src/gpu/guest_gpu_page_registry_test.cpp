#include "device_context.h"
#include "guest_gpu_page_registry.h"
#include "../runtime/physical_memory.h"
#include "../runtime/xmem_bridge.h"
#include <array>
#include <cstring>
#include <vector>

namespace scarlett::gpu {
HRESULT DeviceContext::guest_gpu_page_registry_test() {
    constexpr size_t count = 4;
    constexpr size_t bytes = memory::PhysicalMemory::page_bytes;
    constexpr uint64_t window_bytes = 16ull << 30;
    constexpr uint64_t gpu_first = 4ull << 30;
    constexpr uint64_t gpu_second = 12ull << 30;

    // The host XMem and GPU lifetimes are deliberately independent.
    // The registry pins *both* CPU aliases until the GPU tile is unmapped
    // and the relevant queue fence has completed.
    struct CpuAllocation {
        std::array<uintptr_t, count> frames{};
        bool allocated = false;
        void* first = nullptr;
        void* second = nullptr;
        ~CpuAllocation() {
            if (first) xmem::VirtualFreeBridge(first, 0, MEM_RELEASE);
            if (second) xmem::VirtualFreeBridge(second, 0, MEM_RELEASE);
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
    cpu.first = xmem::XMemVirtualAlloc(
        nullptr, count * bytes, MEM_RESERVE | MEM_64K_PAGES, 0, PAGE_READWRITE);
    cpu.second = xmem::XMemVirtualAlloc(
        nullptr, count * bytes, MEM_RESERVE | MEM_64K_PAGES, 0, PAGE_READWRITE);
    if (!cpu.first || !cpu.second || cpu.first == cpu.second)
        return last_error_ = E_OUTOFMEMORY;
    if (xmem::XMemMapPhysicalPages(
            cpu.first, count, cpu.frames.data()) != cpu.first ||
        xmem::XMemMapPhysicalPages(
            cpu.second, count, cpu.frames.data()) != cpu.second)
        return last_error_ = HRESULT_FROM_WIN32(
            GetLastError() ? GetLastError() : ERROR_GEN_FAILURE);

    auto* first = static_cast<uint8_t*>(cpu.first);
    auto* second = static_cast<uint8_t*>(cpu.second);
    std::vector<uint8_t> initial(count * bytes), modified(count * bytes);
    std::array<uint8_t, bytes> output{};
    for (size_t frame = 0; frame < count; ++frame)
        for (size_t j = 0; j < bytes; ++j) {
            const size_t index = frame * bytes + j;
            initial[index] = static_cast<uint8_t>(
                (j * 37 + (j >> 5) * 11 + frame * 47 + 0x2D) & 255);
            modified[index] = static_cast<uint8_t>(
                (j * 71 + (j >> 7) * 23 + frame * 61 + 0x93) & 255);
        }
    std::memcpy(first, initial.data(), initial.size());
    if (std::memcmp(second, initial.data(), initial.size()) != 0)
        return last_error_ = HRESULT_FROM_WIN32(ERROR_CRC);

    if (!ready()) {
        last_error_ = initialize();
        if (FAILED(last_error_)) return last_error_;
    }
    Microsoft::WRL::ComPtr<ID3D12Device11> native;
    last_error_ = acquire_device11(native.GetAddressOf());
    if (FAILED(last_error_)) return last_error_;
    SparseGpuWindows windows;
    last_error_ = windows.initialize(native.Get(), window_bytes, 2);
    if (FAILED(last_error_)) return last_error_;
    GuestGpuPageRegistry registry(
        memory::PhysicalMemory::instance(), windows, native.Get());

    const auto source = [](size_t i) -> GuestGpuPageRegistry::GpuSlot {
        return {0, gpu_first + i * bytes};
    };
    const auto alias = [](size_t i) -> GuestGpuPageRegistry::GpuSlot {
        return {1, gpu_second + i * bytes};
    };

    if (registry.bind(0, first, second, source(0), alias(0)) != E_INVALIDARG ||
        registry.bind(cpu.frames[0], first, first, source(0), alias(0)) != E_INVALIDARG ||
        registry.bind(cpu.frames[0], first, second + bytes,
                      source(0), alias(0)) !=
            HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS) ||
        registry.pinned_alias_count() != 0)
        return last_error_ = E_FAIL;

    last_error_ = registry.bind(
        cpu.frames[0], first, second, source(0), alias(0));
    if (FAILED(last_error_)) return last_error_;
    if (registry.bind(cpu.frames[0], first, second, source(0), alias(0)) !=
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) ||
        registry.bind(cpu.frames[1], first + bytes, second + bytes,
                      source(0), alias(1)) != HRESULT_FROM_WIN32(ERROR_BUSY) ||
        registry.pinned_alias_count() != 2 ||
        windows.mapped_tile_count() != 2 ||
        registry.physical_heap_count() != 1 ||
        registry.used_heap_tile_count() != 1 ||
        registry.heap_tile_for(cpu.frames[0]) != 0u)
        return last_error_ = E_FAIL;

    for (size_t i = 1; i < count; ++i) {
        last_error_ = registry.bind(
            cpu.frames[i], first + i * bytes, second + i * bytes,
            source(i), alias(i));
        if (FAILED(last_error_)) return last_error_;
    }
    if (registry.binding_count() != count ||
        registry.pinned_alias_count() != 2 * count ||
        windows.mapped_tile_count() != 2 * count ||
        registry.physical_heap_count() != 1 ||
        registry.used_heap_tile_count() != count)
        return last_error_ = E_FAIL;
    // Each frame owns a different 64KiB tile INSIDE one real 4MiB heap.
    for (size_t i = 0; i < count; ++i)
        if (registry.heap_tile_for(cpu.frames[i]) != i)
            return last_error_ = E_FAIL;

    // A registered CPU VA is pinned: unmap and entire-region release must
    // fail without damaging any alias. Scatter unmap must reject the entire
    // batch, not unmap slot 0 then fail on slot 1.
    void* const scatter_addresses[]{first, first + bytes};
    if (xmem::XMemMapPhysicalPages(first, 1, nullptr) != nullptr ||
        GetLastError() != ERROR_BUSY ||
        xmem::XMemMapPhysicalPagesScatter(
            scatter_addresses, 2, nullptr) != FALSE ||
        GetLastError() != ERROR_BUSY ||
        xmem::VirtualFreeBridge(cpu.first, 0, MEM_RELEASE) != FALSE ||
        GetLastError() != ERROR_BUSY ||
        std::memcmp(first, initial.data(), initial.size()) != 0 ||
        std::memcmp(second, initial.data(), initial.size()) != 0)
        return last_error_ = E_FAIL;

    // The four initial per-frame transfers deliberately exercise separate
    // real command submissions on ONE registry-owned D3D12 DIRECT queue.
    if (registry.native_queue_count() != 1) return last_error_ = E_FAIL;
    for (size_t i = 0; i < count; ++i) {
        const auto expected = std::span<const uint8_t>(
            initial.data() + i * bytes, bytes);
        output.fill(0xCC);
        if (registry.transfer(cpu.frames[i], expected, output, false) != S_OK ||
            std::memcmp(output.data(), expected.data(), bytes) != 0 ||
            std::memcmp(first + i * bytes, expected.data(), bytes) != 0 ||
            std::memcmp(second + i * bytes, expected.data(), bytes) != 0)
            return last_error_ = HRESULT_FROM_WIN32(ERROR_CRC);
    }
    if (registry.queue_submission_count() != count ||
        registry.submitted_transfers() != count ||
        registry.transfer(cpu.frames[0],
                          std::span<const uint8_t>(initial.data(), bytes),
                          std::span<uint8_t>(second + bytes, bytes),
                          false) != E_INVALIDARG ||
        registry.queue_submission_count() != count)
        return last_error_ = E_FAIL;

    // Four independent GPU command lists, one ExecuteCommandLists call,
    // one fence and then explicit CPU writeback of ALL four results.
    std::array<std::array<uint8_t, bytes>, count> batch_output{};
    std::array<GuestGpuPageRegistry::PageTransfer, count> batch{};
    for (size_t i = 0; i < count; ++i) {
        batch_output[i].fill(0xA9);
        batch[i] = {cpu.frames[i],
                    std::span<const uint8_t>(modified.data() + i * bytes, bytes),
                    batch_output[i], true};
    }
    // Preflight is atomic for invalid requests: no GPU submission or output
    // mutation, including duplicate frames and unsafe CPU alias outputs.
    auto invalid = batch;
    invalid[1].frame = cpu.frames[0];
    if (registry.transfer_batch(invalid) != E_INVALIDARG)
        return last_error_ = E_FAIL;
    invalid = batch;
    invalid[3].frame = 0xFFFFFFFFu;
    if (registry.transfer_batch(invalid) !=
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        return last_error_ = E_FAIL;
    invalid = batch;
    invalid[2].output = std::span<uint8_t>(second + 2 * bytes, bytes);
    if (registry.transfer_batch(invalid) != E_INVALIDARG)
        return last_error_ = E_FAIL;
    invalid = batch;
    invalid[1].output = batch_output[0]; // overlapping results
    if (registry.transfer_batch(invalid) != E_INVALIDARG)
        return last_error_ = E_FAIL;
    invalid = batch;
    invalid[1].output = std::span<uint8_t>(batch_output[1].data(), bytes - 1);
    if (registry.transfer_batch(invalid) != E_INVALIDARG ||
        registry.transfer_batch({}) != E_INVALIDARG ||
        registry.queue_submission_count() != count ||
        batch_output[0][0] != 0xA9 ||
        std::memcmp(first, initial.data(), initial.size()) != 0 ||
        std::memcmp(second, initial.data(), initial.size()) != 0)
        return last_error_ = E_FAIL;

    last_error_ = registry.transfer_batch(batch);
    if (FAILED(last_error_)) return last_error_;
    if (registry.submitted_transfers() != count * 2 ||
        registry.queue_submission_count() != count + 1 ||
        registry.native_queue_count() != 1 ||
        registry.transfer(0xFFFFFFFFu, modified, output, false) !=
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        return last_error_ = E_FAIL;
    for (size_t i = 0; i < count; ++i) {
        const auto changed = modified.data() + i * bytes;
        if (std::memcmp(batch_output[i].data(), changed, bytes) != 0 ||
            std::memcmp(first + i * bytes, changed, bytes) != 0 ||
            std::memcmp(second + i * bytes, changed, bytes) != 0)
            return last_error_ = HRESULT_FROM_WIN32(ERROR_CRC);
    }

    // Reuse all four native command allocators for a SECOND batch.
    // This checks the cross-resource aliasing barriers for repeat submissions
    // and verifies no implicit CPU coherency when writeback is disabled.
    std::array<std::array<uint8_t, bytes>, count> second_batch_output{};
    auto second_batch = batch;
    for (size_t i = 0; i < count; ++i) {
        second_batch[i].input =
            std::span<const uint8_t>(initial.data() + i * bytes, bytes);
        second_batch[i].output = second_batch_output[i];
        second_batch[i].writeback_cpu = false;
    }
    last_error_ = registry.transfer_batch(second_batch);
    if (FAILED(last_error_) ||
        registry.submitted_transfers() != count * 3 ||
        registry.queue_submission_count() != count + 2)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
    for (size_t i = 0; i < count; ++i) {
        if (std::memcmp(second_batch_output[i].data(),
                        initial.data() + i * bytes, bytes) != 0 ||
            std::memcmp(first + i * bytes,
                        modified.data() + i * bytes, bytes) != 0 ||
            std::memcmp(second + i * bytes,
                        modified.data() + i * bytes, bytes) != 0)
            return last_error_ = HRESULT_FROM_WIN32(ERROR_CRC);
    }

    // Retire one frame then rebind to prove GPU slot and CPU alias pins are
    // released independently of the other three live registrations.
    last_error_ = registry.unbind(cpu.frames[0]);
    if (FAILED(last_error_)) return last_error_;
    if (registry.unbind(cpu.frames[0]) !=
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
        registry.pinned_alias_count() != 2 * (count - 1) ||
        windows.mapped_tile_count() != 2 * (count - 1) ||
        registry.used_heap_tile_count() != count - 1 ||
        registry.physical_heap_count() != 1 ||
        registry.heap_tile_for(cpu.frames[0]).has_value())
        return last_error_ = E_FAIL;

    // Slot 0 is now unpinned but slot 1 remains pinned. The atomic
    // two-slot scatter-unmap must reject the request BEFORE unmapping the
    // free first slot. Then a single-slot unmap/remap of frame 0 is allowed.
    if (xmem::XMemMapPhysicalPagesScatter(
            scatter_addresses, 2, nullptr) != FALSE ||
        GetLastError() != ERROR_BUSY ||
        std::memcmp(first, modified.data(), modified.size()) != 0 ||
        xmem::XMemMapPhysicalPages(first, 1, nullptr) != first ||
        xmem::XMemMapPhysicalPages(first, 1, cpu.frames.data()) != first ||
        std::memcmp(first, modified.data(), modified.size()) != 0)
        return last_error_ = E_FAIL;

    last_error_ = registry.bind(
        cpu.frames[0], first, second, source(0), alias(0));
    if (FAILED(last_error_)) return last_error_;
    if (registry.pinned_alias_count() != count * 2 ||
        windows.mapped_tile_count() != count * 2 ||
        registry.physical_heap_count() != 1 ||
        registry.used_heap_tile_count() != count ||
        registry.heap_tile_for(cpu.frames[0]) != 0u)
        return last_error_ = E_FAIL;
    for (size_t i = 1; i < count; ++i)
        if (registry.heap_tile_for(cpu.frames[i]) != i)
            return last_error_ = E_FAIL;

    // Reuse the freed native GPU heap tile 0 without disturbing tiles 1-3.
    // Four more generated page readbacks share one fence/submission, and
    // CPU aliases must continue to hold the previous explicitly written data.
    std::array<std::array<uint8_t, bytes>, count> reused_output{};
    auto reused_batch = batch;
    for (size_t i = 0; i < count; ++i) {
        reused_batch[i].input =
            std::span<const uint8_t>(initial.data() + i * bytes, bytes);
        reused_batch[i].output = reused_output[i];
        reused_batch[i].writeback_cpu = false;
    }
    last_error_ = registry.transfer_batch(reused_batch);
    if (FAILED(last_error_) ||
        registry.submitted_transfers() != count * 4 ||
        registry.queue_submission_count() != count + 3 ||
        registry.used_heap_tile_count() != count)
        return last_error_ = FAILED(last_error_) ? last_error_ : E_FAIL;
    for (size_t i = 0; i < count; ++i)
        if (std::memcmp(reused_output[i].data(),
                        initial.data() + i * bytes, bytes) != 0 ||
            std::memcmp(first + i * bytes,
                        modified.data() + i * bytes, bytes) != 0 ||
            std::memcmp(second + i * bytes,
                        modified.data() + i * bytes, bytes) != 0)
            return last_error_ = HRESULT_FROM_WIN32(ERROR_CRC);

    last_error_ = registry.clear();
    if (FAILED(last_error_) ||
        registry.binding_count() != 0 ||
        registry.pinned_alias_count() != 0 ||
        windows.mapped_tile_count() != 0 ||
        registry.physical_heap_count() != 0 ||
        registry.used_heap_tile_count() != 0 ||
        std::memcmp(first, modified.data(), modified.size()) != 0 ||
        std::memcmp(second, modified.data(), modified.size()) != 0)
        return last_error_ = E_FAIL;

    submitted_ += registry.submitted_transfers();
    registry_queue_submissions_ = registry.queue_submission_count();
    windows.clear();
    return last_error_ = S_OK;
}
} // namespace scarlett::gpu
