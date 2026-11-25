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
        // Windows VirtualFree is NOT allowed to bypass registered
        // page-backed XMemAlloc memory, even when no pins are live.
        SetLastError(ERROR_SUCCESS);
        if (VirtualFreeBridge(pages.pointer, 0, MEM_RELEASE) != FALSE ||
            GetLastError() != ERROR_INVALID_ADDRESS)
            return false;
        if (!snapshot_owned_xmem_utf16(
                static_cast<const wchar_t*>(pages.pointer), 240, output) ||
            output != path)
            return false;

        {
            std::shared_ptr<HostXMemWritePin> pin;
            const size_t before = active_xmem_write_pin_count();
            if (pin_owned_xmem_destination(pages.pointer, 128, pin) !=
                    S_OK || !pin ||
                active_xmem_write_pin_count() != before + 1)
                return false;
            SetLastError(ERROR_SUCCESS);
            if (VirtualFreeBridge(pages.pointer, 0, MEM_RELEASE) != FALSE ||
                GetLastError() != ERROR_BUSY)
                return false;
            SetLastError(ERROR_SUCCESS);
            XMemFreeDefault(pages.pointer, 0);
            if (GetLastError() != ERROR_BUSY) return false;
            pin.reset();
            if (active_xmem_write_pin_count() != before) return false;
        }

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

        // Owned, writable host heap ranges can be pinned for asynchronous
        // output. XMemFree MUST refuse to release the backing allocation
        // until the final pin is gone, including when called from a
        // different thread.
        {
            HostHeapOwner writable{XMemAlloc(4096, 0)};
            if (!writable.pointer) return false;
            auto* target = static_cast<uint8_t*>(writable.pointer) + 128;
            const size_t before_pins = active_xmem_write_pin_count();
            std::shared_ptr<HostXMemWritePin> first, second, invalid;
            if (pin_owned_xmem_destination(target, 256, first) != S_OK ||
                !first || first->size() != 256 ||
                first->address() != target ||
                active_xmem_write_pin_count() != before_pins + 1 ||
                pin_owned_xmem_destination(target + 64, 128, second) != S_OK ||
                active_xmem_write_pin_count() != before_pins + 2)
                return false;
            if (pin_owned_xmem_destination(
                    target + 3900, 512, invalid) == S_OK || invalid ||
                pin_owned_xmem_destination(
                    copied.data(), 64, invalid) == S_OK || invalid)
                return false;
            SetLastError(ERROR_SUCCESS);
            XMemFree(writable.pointer, 0);
            if (GetLastError() != ERROR_BUSY) return false;
            std::array<uint8_t, 256> signal{};
            std::fill(signal.begin(), signal.end(), uint8_t{0x7c});
            if (first->write(signal.data(), 255) != E_INVALIDARG ||
                first->write(signal.data(), signal.size()) != S_OK ||
                !std::equal(signal.begin(), signal.end(), target))
                return false;
            first.reset();
            SetLastError(ERROR_SUCCESS);
            XMemFree(writable.pointer, 0);
            if (GetLastError() != ERROR_BUSY ||
                active_xmem_write_pin_count() != before_pins + 1)
                return false;
            second.reset();
            if (active_xmem_write_pin_count() != before_pins)
                return false;
            writable.release();
            if (pin_owned_xmem_destination(target, 256, invalid) == S_OK ||
                invalid || active_xmem_write_pin_count() != before_pins)
                return false;
        }

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
