#include "xmem_bridge.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

namespace scarlett::xmem {
namespace {
struct HostHeapOwner {
    void* pointer = nullptr;
    ~HostHeapOwner() { if (pointer) XMemFree(pointer, 0); }
    void release() {
        if (pointer) XMemFree(pointer, 0);
        pointer = nullptr;
    }
};
}

bool verify_xmem_heap_snapshots_fixture() noexcept {
    try {
        // Real XMemAlloc, not an ordinary process heap allocation.
        HostHeapOwner heap{XMemAlloc(4096, 0)};
        if (!heap.pointer) return false;
        auto* bytes = static_cast<uint8_t*>(heap.pointer);
        for (size_t i = 0; i < 4096; ++i)
            bytes[i] = static_cast<uint8_t>((i * 19u + 3) & 255u);
        std::array<uint8_t, 130> copied{};
        if (!snapshot_owned_xmem_allocation(bytes + 4019, 77,
                                             copied.data()) ||
            !std::equal(copied.begin(), copied.begin() + 77,
                        bytes + 4019))
            return false;

        // A read cannot cross the end, wrap an address, or include a
        // foreign unregistered buffer. Failure never touches output.
        copied.fill(0xcd);
        const auto not_modified = [&] {
            return std::all_of(copied.begin(), copied.end(),
                [](uint8_t c) { return c == 0xcd; });
        };
        if (snapshot_owned_xmem_allocation(
                bytes + 4019, 78, copied.data()) || !not_modified() ||
            snapshot_owned_xmem_allocation(
                reinterpret_cast<const void*>(UINTPTR_MAX - 7),
                100, copied.data()) || !not_modified() ||
            snapshot_owned_xmem_allocation(
                copied.data(), 20, copied.data()) || !not_modified())
            return false;

        // A valid path is a UTF-16 string that fits entirely inside one
        // recorded heap allocation; the source is snapshotted by value.
        constexpr wchar_t path[] = L"pack0:/generated.bin";
        auto* guest_path = reinterpret_cast<wchar_t*>(bytes + 256);
        std::memcpy(guest_path, path, sizeof(path));
        std::wstring output = L"previous";
        if (!snapshot_owned_xmem_utf16(guest_path, 240, output) ||
            output != path)
            return false;
        std::memset(guest_path, 0, sizeof(path));
        if (output != path) return false;

        // Unterminated and out-of-bounds strings preserve the previous
        // output; neither the allocator nor any foreign pointer is read.
        auto* end = reinterpret_cast<wchar_t*>(bytes + 4092);
        end[0] = L'x';
        end[1] = L'y';
        output = L"untouched";
        if (snapshot_owned_xmem_utf16(end, 240, output) ||
            output != L"untouched" ||
            snapshot_owned_xmem_utf16(
                reinterpret_cast<const wchar_t*>(UINTPTR_MAX - 7),
                240, output) || output != L"untouched")
            return false;

        // A high-attribute page-backed XMemAlloc path must also work.
        HostHeapOwner pages{XMemAllocDefault(
            4096, uint64_t{1} << 29)};
        if (!pages.pointer) return false;
        std::memcpy(pages.pointer, path, sizeof(path));
        if (!snapshot_owned_xmem_utf16(
                static_cast<const wchar_t*>(pages.pointer), 240, output) ||
            output != path)
            return false;

        // The separate XMemVirtualAlloc reservations are explicitly
        // excluded: they may have been partially committed or protected.
        void* reserved = XMemVirtualAlloc(
            nullptr, 4096, MEM_RESERVE | MEM_COMMIT, 0, PAGE_READWRITE);
        if (!reserved) return false;
        std::memcpy(reserved, path, sizeof(path));
        output = L"unchanged";
        const bool rejected = !snapshot_owned_xmem_utf16(
            static_cast<const wchar_t*>(reserved), 240, output) &&
            output == L"unchanged";
        const BOOL released = VirtualFreeBridge(
            reserved, 0, MEM_RELEASE);
        if (!rejected || !released) return false;

        // The registry mutex also protects against concurrent XMemFree.
        // The consumer sees an exact complete copy, or a failure; it
        // never accesses freed storage or a partially freed allocation.
        HostHeapOwner concurrent{XMemAlloc(4096, 0)};
        if (!concurrent.pointer) return false;
        std::memset(concurrent.pointer, 0x69, 4096);
        const void* const candidate = concurrent.pointer;
        std::atomic<bool> valid{true};
        std::jthread reader([&] {
            for (unsigned n = 0; n < 1000; ++n) {
                std::array<uint8_t, 256> copy{};
                if (snapshot_owned_xmem_allocation(
                        candidate, copy.size(), copy.data()) &&
                    !std::all_of(copy.begin(), copy.end(),
                        [](uint8_t c) { return c == 0x69; }))
                    valid.store(false);
            }
        });
        concurrent.release();
        reader.join();
        copied.fill(0xcd);
        if (!valid.load() ||
            snapshot_owned_xmem_allocation(
                candidate, copied.size(), copied.data()) ||
            !not_modified())
            return false;

        pages.release();
        heap.release();
        output = L"keep";
        copied.fill(0xcd);
        if (snapshot_owned_xmem_allocation(
                guest_path, 2, copied.data()) || !not_modified() ||
            snapshot_owned_xmem_utf16(
                guest_path, 240, output) || output != L"keep")
            return false;
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::xmem
