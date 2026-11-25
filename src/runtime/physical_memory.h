#pragma once
#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace scarlett::memory {
// Original, host-backed Xbox 64 KiB physical page compatibility prototype.
// A page number is an opaque synthetic identifier (in 4 KiB frame units).
// It is NOT a hardware physical address and is not GPU mapped.
class PhysicalMemory final {
public:
    static constexpr size_t page_bytes = 64 * 1024;
    static PhysicalMemory& instance();

    BOOL allocate_pages(uintptr_t flags, uintptr_t* inout_count, uintptr_t* out_page_numbers);
    BOOL free_pages(size_t count, const uintptr_t* page_numbers);
    void* reserve_alias_region(void* requested_address, size_t bytes);
    void* map_pages(void* destination, size_t count, const uintptr_t* page_numbers);
    // Map or unmap a scattered list of 64 KiB VA slots across reservations.
    // All destinations must be ScarlettVI-managed, aligned placeholders.
    BOOL map_scatter(void* const* destinations, size_t count, const uintptr_t* page_numbers);
    BOOL release_alias_region(void* base);
    bool owns_alias_region_base(void* base) const;
    // Pin an exact 64KiB CPU alias mapped to this synthetic frame number.
    // Pinned slots cannot be unmapped or released until unpinned. This
    // safeguards host staging against a stale VA; it is not Xbox GPU residency.
    bool pin_mapped_alias(void* address, uintptr_t physical_frame);
    bool unpin_mapped_alias(void* address, uintptr_t physical_frame);
    // Only expose ranges currently backed by ScarlettVI-owned XMem CPU
    // aliases. Both queries hold the registry mutex and reject any
    // unmapped page; neither attempts to inspect arbitrary guest memory.
    // Returned frames are revalidated by the independent pin operation.
    struct MappedAliasSpan {
        static constexpr size_t kMaxFrames = 17; // unaligned 1MiB
        std::array<uintptr_t, kMaxFrames> frames{};
        size_t count = 0;
    };
    bool resolve_mapped_alias_span(const void* source, size_t bytes,
                                   MappedAliasSpan& result) const noexcept;
    bool snapshot_mapped_alias(const void* source, size_t bytes,
                               void* host_output) const noexcept;
    // Copy a NUL-terminated UTF-16 path from owned CPU aliases while
    // holding the registry mutex. Never reads beyond the terminator,
    // verifies both pages for straddling code units, and refuses raw
    // unregistered host/guest pointers. Windows wchar_t is 16 bits.
    // The output is unchanged on failure, including an absent terminator.
    bool snapshot_mapped_alias_utf16(
        const wchar_t* guest_source, size_t max_chars,
        std::wstring& output) const noexcept;
    size_t pinned_alias_count() const;
    size_t allocated_page_count() const;

    PhysicalMemory(const PhysicalMemory&) = delete;
    PhysicalMemory& operator=(const PhysicalMemory&) = delete;
private:
    PhysicalMemory() = default;
    ~PhysicalMemory();
    struct Page {
        HANDLE section = nullptr;
        size_t aliases = 0;
    };
    struct Region {
        size_t page_count = 0;
        std::vector<uintptr_t> mapped_pages; // 0 means unoccupied placeholder
    };
    bool unmap_one_unlocked(void* address, Region& region, size_t index);
    bool split_placeholder_slot_unlocked(void* address);
    bool release_region_unlocked(void* address, Region& region);
    bool resolve_mapped_alias_span_unlocked(
        const void* source, size_t bytes, MappedAliasSpan& result) const noexcept;
    mutable std::mutex mutex_;
    std::unordered_map<uintptr_t, Page> pages_;
    std::unordered_map<void*, Region> regions_;
    std::unordered_map<void*, size_t> pinned_aliases_;
    uintptr_t next_physical_frame_ = 0x100000;
};
} // namespace scarlett::memory
