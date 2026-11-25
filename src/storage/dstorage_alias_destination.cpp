#include "dstorage_alias_destination.h"
#include "../runtime/physical_memory.h"
#include <cstring>
#include <new>
#include <utility>

namespace scarlett::storage {

HRESULT HostDStorageAliasDestination::pin(
    void* destination, uintptr_t physical_frame, size_t bytes,
    std::shared_ptr<HostDStorageAliasDestination>& output) noexcept {
    output.reset();
    if (!destination || !physical_frame || !bytes || bytes > page_bytes)
        return E_INVALIDARG;
    const uintptr_t address = reinterpret_cast<uintptr_t>(destination);
    const size_t offset = static_cast<size_t>(address & (page_bytes - 1));
    // Overflow-free check. Never cross a 64KiB mapped XMem page.
    if (bytes > page_bytes - offset)
        return HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
    void* const page = reinterpret_cast<void*>(
        address & ~static_cast<uintptr_t>(page_bytes - 1));
    try {
        // Allocate all host bookkeeping before acquiring the mapping pin.
        // No allocation may fail after pin_mapped_alias succeeds.
        auto owner = std::shared_ptr<HostDStorageAliasDestination>(
            new HostDStorageAliasDestination(page, physical_frame,
                                             destination, bytes));
        auto& memory = memory::PhysicalMemory::instance();
        if (!memory.pin_mapped_alias(page, physical_frame)) {
            const DWORD error = GetLastError();
            return HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_ADDRESS);
        }
        owner->pinned_ = true;
        output = std::move(owner);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HostDStorageAliasDestination::~HostDStorageAliasDestination() noexcept {
    if (pinned_)
        (void)memory::PhysicalMemory::instance().unpin_mapped_alias(
            page_base_, frame_);
}

HRESULT HostDStorageAliasDestination::write(
    const uint8_t* source, size_t bytes) noexcept {
    if (!pinned_ || !source || bytes != bytes_)
        return E_INVALIDARG;
    // The physical alias registry refuses remap/unmap/release while pinned,
    // so destination_ is backed by a live, real Windows file mapping.
    std::memcpy(destination_, source, bytes);
    return S_OK;
}
} // namespace scarlett::storage
