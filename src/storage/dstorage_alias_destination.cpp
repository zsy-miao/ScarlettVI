#include "dstorage_alias_destination.h"
#include "../runtime/physical_memory.h"
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace scarlett::storage {

HRESULT HostDStorageAliasDestination::pin(
    void* destination, uintptr_t physical_frame, size_t bytes,
    std::shared_ptr<HostDStorageAliasDestination>& output) noexcept {
    // Preserve the one-page entry point: a crossing request must still
    // fail instead of reading beyond the supplied frame identity.
    return pin_pages(destination, &physical_frame, 1, bytes, output);
}

HRESULT HostDStorageAliasDestination::pin_pages(
    void* destination, const uintptr_t* physical_frames,
    size_t frame_count, size_t bytes,
    std::shared_ptr<HostDStorageAliasDestination>& output) noexcept {
    output.reset();
    // The file worker already caps read sizes at 1MiB. Match that quota,
    // including the maximum 17 intersected pages for an unaligned start.
    constexpr size_t kMaxReadBytes = 1024 * 1024;
    constexpr size_t kMaxPageCount = kMaxReadBytes / page_bytes + 1;
    if (!destination || !physical_frames || !frame_count ||
        frame_count > kMaxPageCount || !bytes || bytes > kMaxReadBytes)
        return E_INVALIDARG;
    const uintptr_t address = reinterpret_cast<uintptr_t>(destination);
    const size_t offset = static_cast<size_t>(address & (page_bytes - 1));
    if (bytes - 1 > std::numeric_limits<uintptr_t>::max() - address)
        return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
    const size_t required_pages = 1 + (offset + bytes - 1) / page_bytes;
    if (required_pages != frame_count)
        return HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
    const uintptr_t start = address &
        ~static_cast<uintptr_t>(page_bytes - 1);
    try {
        auto owner = std::shared_ptr<HostDStorageAliasDestination>(
            new HostDStorageAliasDestination(destination, bytes));
        // Allocate the entire bookkeeping vector before acquiring a pin.
        // From here onward, a failed pin has an allocation-free rollback.
        owner->pins_.reserve(required_pages);
        for (size_t i = 0; i < required_pages; ++i) {
            if (!physical_frames[i]) return E_INVALIDARG;
            owner->pins_.push_back({
                reinterpret_cast<void*>(start + i * page_bytes),
                physical_frames[i]
            });
        }
        auto& memory = memory::PhysicalMemory::instance();
        for (const Pin& pin : owner->pins_) {
            if (!memory.pin_mapped_alias(pin.page_base, pin.frame)) {
                const DWORD error = GetLastError();
                // ~HostDStorageAliasDestination unpins the successfully
                // acquired prefix even when later validation failed.
                return HRESULT_FROM_WIN32(
                    error ? error : ERROR_INVALID_ADDRESS);
            }
            ++owner->pinned_count_;
        }
        output = std::move(owner);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HostDStorageAliasDestination::~HostDStorageAliasDestination() noexcept {
    auto& memory = memory::PhysicalMemory::instance();
    for (size_t i = pinned_count_; i != 0; --i)
        (void)memory.unpin_mapped_alias(
            pins_[i - 1].page_base, pins_[i - 1].frame);
}

HRESULT HostDStorageAliasDestination::write(
    const uint8_t* source, size_t bytes) noexcept {
    if (pins_.empty() || pinned_count_ != pins_.size() ||
        !source || bytes != bytes_)
        return E_INVALIDARG;
    // Every intersected virtual page is pinned in PhysicalMemory.
    // Its alias remap/unmap/release paths reject pinned slots, so this
    // contiguous virtual-address range stays backed during the copy.
    std::memcpy(destination_, source, bytes);
    return S_OK;
}
} // namespace scarlett::storage
