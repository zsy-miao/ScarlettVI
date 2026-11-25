#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace scarlett::storage {

// HOST-ONLY DirectStorage destination pin for contiguous XMem CPU aliases.
// A trusted test supplies the exact physical frame for EACH intersected
// 64KiB page. All pins must succeed before I/O may start; partial failures
// roll back. Mappings cannot be unmapped while a worker owns the pins.
// Not a console GPU destination, page-list mode, or untrusted guest API.
class HostDStorageAliasDestination final {
public:
    static constexpr size_t page_bytes = 64 * 1024;

    static HRESULT pin(void* destination, uintptr_t physical_frame,
                       size_t bytes,
                       std::shared_ptr<HostDStorageAliasDestination>& output)
                       noexcept;
    // Explicit host-only frame list, ordered by ascending 64KiB CPU VA
    // slots. Cannot cross an unregistered or incorrectly mapped page.
    static HRESULT pin_pages(void* destination,
                             const uintptr_t* physical_frames,
                             size_t frame_count, size_t bytes,
                             std::shared_ptr<HostDStorageAliasDestination>& output)
                             noexcept;
    ~HostDStorageAliasDestination() noexcept;
    HostDStorageAliasDestination(const HostDStorageAliasDestination&) = delete;
    HostDStorageAliasDestination& operator=(const HostDStorageAliasDestination&) = delete;

    size_t size() const noexcept { return bytes_; }
    void* address() const noexcept { return destination_; }
    HRESULT write(const uint8_t* source, size_t bytes) noexcept;

private:
    struct Pin {
        void* page_base;
        uintptr_t frame;
    };
    HostDStorageAliasDestination(void* destination, size_t bytes) noexcept
        : destination_(destination), bytes_(bytes) {}
    std::vector<Pin> pins_;
    size_t pinned_count_ = 0;
    void* destination_ = nullptr;
    size_t bytes_ = 0;
};

} // namespace scarlett::storage
