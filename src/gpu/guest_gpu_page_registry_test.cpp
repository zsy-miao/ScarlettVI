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
        windows.mapped_tile_count() != 2)
        return last_error_ = E_FAIL;

    for (size_t i = 1; i < count; ++i) {
        last_error_ = registry.bind(
            cpu.frames[i], first + i * bytes, second + i * bytes,
            source(i), alias(i));
        if (FAILED(last_error_)) return last_error_;
    }
    if (registry.binding_count() != count ||
        registry.pinned_alias_count() != 2 * count ||
        windows.mapped_tile_count() != 2 * count)
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

    // Each guest frame owns an independently retained physical D3D12 tile.
    // First transfer does NOT change either CPU alias; the second explicitly
    // writes the GPU readback into the pinned mirror alias.
    for (size_t i = 0; i < count; ++i) {
        const auto frame = cpu.frames[i];
        const auto expected = std::span<const uint8_t>(
            initial.data() + i * bytes, bytes);
        output.fill(0xCC);
        if (registry.transfer(frame, expected, output, false) != S_OK ||
            std::memcmp(output.data(), expected.data(), bytes) != 0 ||
            std::memcmp(first + i * bytes, expected.data(), bytes) != 0 ||
            std::memcmp(second + i * bytes, expected.data(), bytes) != 0)
            return last_error_ = HRESULT_FROM_WIN32(ERROR_CRC);

        const auto changed = std::span<const uint8_t>(
            modified.data() + i * bytes, bytes);
        if (registry.transfer(frame, changed,
                              std::span<uint8_t>(output.data(), bytes - 1),
                              true) != E_INVALIDARG ||
            registry.transfer(frame, changed,
                              std::span<uint8_t>(second + i * bytes, bytes),
                              false) != E_INVALIDARG ||
            registry.transfer(frame, changed, output, true) != S_OK ||
            std::memcmp(output.data(), changed.data(), bytes) != 0 ||
            std::memcmp(first + i * bytes, changed.data(), bytes) != 0 ||
            std::memcmp(second + i * bytes, changed.data(), bytes) != 0)
            return last_error_ = HRESULT_FROM_WIN32(ERROR_CRC);
    }
    if (registry.submitted_transfers() != count * 2 ||
        registry.transfer(0xFFFFFFFFu, modified, output, false) !=
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        return last_error_ = E_FAIL;

    // Retire one frame then rebind to prove GPU slot and CPU alias pins are
    // released independently of the other three live registrations.
    last_error_ = registry.unbind(cpu.frames[0]);
    if (FAILED(last_error_)) return last_error_;
    if (registry.unbind(cpu.frames[0]) !=
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
        registry.pinned_alias_count() != 2 * (count - 1) ||
        windows.mapped_tile_count() != 2 * (count - 1))
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
        windows.mapped_tile_count() != count * 2)
        return last_error_ = E_FAIL;

    last_error_ = registry.clear();
    if (FAILED(last_error_) ||
        registry.binding_count() != 0 ||
        registry.pinned_alias_count() != 0 ||
        windows.mapped_tile_count() != 0 ||
        std::memcmp(first, modified.data(), modified.size()) != 0 ||
        std::memcmp(second, modified.data(), modified.size()) != 0)
        return last_error_ = E_FAIL;

    submitted_ += registry.submitted_transfers();
    windows.clear();
    return last_error_ = S_OK;
}
} // namespace scarlett::gpu
