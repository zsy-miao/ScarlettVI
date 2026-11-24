#include "physical_memory.h"
#include <algorithm>
#include <limits>
#include <memoryapi.h>
#include <new>
#include <unordered_set>

namespace scarlett::memory {
namespace {
using VirtualAlloc2Fn = PVOID (WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                        MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID (WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG,
                                       ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using UnmapViewOfFile2Fn = BOOL (WINAPI*)(HANDLE, PVOID, ULONG);
struct PlaceholderApi {
    VirtualAlloc2Fn reserve = nullptr;
    MapViewOfFile3Fn map = nullptr;
    UnmapViewOfFile2Fn unmap = nullptr;
    PlaceholderApi() {
        HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
        if (!kernel) kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel) return;
        reserve = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernel, "VirtualAlloc2"));
        map = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernel, "MapViewOfFile3"));
        unmap = reinterpret_cast<UnmapViewOfFile2Fn>(GetProcAddress(kernel, "UnmapViewOfFile2"));
    }
    bool available() const noexcept { return reserve && map && unmap; }
};
PlaceholderApi& host() {
    static PlaceholderApi api;
    return api;
}
bool aligned(size_t number) { return (number & (PhysicalMemory::page_bytes - 1)) == 0; }
}

PhysicalMemory& PhysicalMemory::instance() {
    static PhysicalMemory state;
    return state;
}

BOOL PhysicalMemory::allocate_pages(uintptr_t flags, uintptr_t* count,
                                    uintptr_t* numbers) {
    // The public Microsoft GDK sample documents MEM_64K_PAGES, an IN/OUT
    // count, an output page-number vector, and a BOOL result. Clustered 2 MiB
    // allocation and arbitrary memory hints require separate implementations.
    if (!count || !numbers || !*count || flags != MEM_64K_PAGES ||
        *count > 65536) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    std::lock_guard lock(mutex_);
    const uintptr_t count_to_allocate = *count;
    if (count_to_allocate > (std::numeric_limits<uintptr_t>::max() - next_physical_frame_) / 16) {
        SetLastError(ERROR_ARITHMETIC_OVERFLOW);
        return FALSE;
    }
    std::vector<std::pair<uintptr_t, HANDLE>> prepared;
    try {
        prepared.reserve(static_cast<size_t>(count_to_allocate));
        for (uintptr_t i = 0; i < count_to_allocate; ++i) {
            HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                               0, static_cast<DWORD>(page_bytes), nullptr);
            if (!section) {
                const DWORD error = GetLastError();
                for (const auto& [frame, handle] : prepared) CloseHandle(handle);
                SetLastError(error);
                return FALSE;
            }
            prepared.emplace_back(next_physical_frame_ + i * 16, section);
        }
        pages_.reserve(pages_.size() + prepared.size());
        for (const auto& [frame, section] : prepared)
            pages_.emplace(frame, Page{section, 0});
        for (size_t i = 0; i < prepared.size(); ++i)
            numbers[i] = prepared[i].first;
        next_physical_frame_ += count_to_allocate * 16;
    } catch (...) {
        for (const auto& [frame, section] : prepared) {
            pages_.erase(frame);
            CloseHandle(section);
        }
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    return TRUE;
}

BOOL PhysicalMemory::free_pages(size_t count, const uintptr_t* numbers) {
    if (!count || !numbers) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::lock_guard lock(mutex_);
    std::unordered_set<uintptr_t> unique;
    try {
        unique.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (!unique.emplace(numbers[i]).second) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            const auto it = pages_.find(numbers[i]);
            if (it == pages_.end() || it->second.aliases != 0) {
                SetLastError(ERROR_BUSY);
                return FALSE;
            }
        }
    } catch (...) {
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    for (const uintptr_t frame : unique) {
        CloseHandle(pages_.at(frame).section);
        pages_.erase(frame);
    }
    return TRUE;
}

void* PhysicalMemory::reserve_alias_region(void* requested_address, size_t size) {
    if (!size || !aligned(size) || (requested_address &&
        !aligned(reinterpret_cast<uintptr_t>(requested_address)))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (!host().available()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    void* address = host().reserve(nullptr, requested_address, size,
                                    MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                    PAGE_NOACCESS, nullptr, 0);
    if (!address) return nullptr;
    const size_t count = size / page_bytes;
    // Do NOT split the entire reservation here. The title reserves virtual
    // windows of 16-24 GiB, for which eager 64KiB splitting would invoke
    // VirtualFree hundreds of thousands of times. A single Windows
    // placeholder costs no committed RAM; split only pages when mapped.
    try {
        Region region;
        region.page_count = count;
        region.mapped_pages.resize(count);
        std::lock_guard lock(mutex_);
        const auto [it, added] = regions_.emplace(address, std::move(region));
        if (!added) {
            VirtualFree(address, 0, MEM_RELEASE);
            SetLastError(ERROR_ALREADY_EXISTS);
            return nullptr;
        }
    } catch (...) {
        VirtualFree(address, 0, MEM_RELEASE);
        SetLastError(ERROR_OUTOFMEMORY);
        return nullptr;
    }
    return address;
}

// Separates a single 64KiB slot from its host placeholder allocation by at
// most two O(1) splits, regardless of the size of the surrounding reservation.
// Windows documents MEM_PRESERVE_PLACEHOLDER as never releasing the VA.
bool PhysicalMemory::split_placeholder_slot_unlocked(void* address) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info))
        return false;
    const uintptr_t location = reinterpret_cast<uintptr_t>(address);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(info.AllocationBase);
    const uintptr_t reported_begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
    if (!begin || !aligned(location) || info.State != MEM_RESERVE ||
        reported_begin > location || location < begin ||
        info.RegionSize > std::numeric_limits<uintptr_t>::max() - reported_begin) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return false;
    }
    const uintptr_t end = reported_begin + info.RegionSize;
    if (location > end || end - location < page_bytes) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return false;
    }
    if (location > begin) {
        // Splitting at the BEGINNING of the current allocation yields
        // [begin,location) and [location,end), preserving both placeholders.
        if (!VirtualFree(reinterpret_cast<void*>(begin), location - begin,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            return false;
    }
    if (end - location > page_bytes) {
        // Isolate [location,location+64KiB) from the remaining range.
        if (!VirtualFree(address, page_bytes,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            return false;
    }
    return true;
}

bool PhysicalMemory::unmap_one_unlocked(void* address, Region& region, size_t index) {
    const uintptr_t page_number = region.mapped_pages[index];
    if (!page_number) return true;
    if (!host().unmap(GetCurrentProcess(), address, MEM_PRESERVE_PLACEHOLDER))
        return false;
    const auto it = pages_.find(page_number);
    if (it != pages_.end() && it->second.aliases != 0) --it->second.aliases;
    region.mapped_pages[index] = 0;
    return true;
}

void* PhysicalMemory::map_pages(void* target, size_t count,
                                const uintptr_t* numbers) {
    if (!target || count == 0 || !aligned(reinterpret_cast<uintptr_t>(target))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (!host().available()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    std::lock_guard lock(mutex_);
    Region* region = nullptr;
    size_t begin = 0;
    for (auto& [base, candidate] : regions_) {
        const uintptr_t first = reinterpret_cast<uintptr_t>(base);
        const uintptr_t current = reinterpret_cast<uintptr_t>(target);
        if (current >= first && ((current - first) / page_bytes) < candidate.page_count) {
            begin = (current - first) / page_bytes;
            if (count <= candidate.page_count - begin)
                region = &candidate;
            break;
        }
    }
    if (!region) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return nullptr;
    }
    if (!numbers) {
        for (size_t i = 0; i < count; ++i) {
            void* at = static_cast<uint8_t*>(target) + i * page_bytes;
            if (!unmap_one_unlocked(at, *region, begin + i))
                return nullptr;
        }
        return target;
    }

    // Validate the WHOLE request before changing virtual address mappings.
    for (size_t i = 0; i < count; ++i) {
        if (region->mapped_pages[begin + i] || pages_.find(numbers[i]) == pages_.end()) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        void* at = static_cast<uint8_t*>(target) + i * page_bytes;
        if (!split_placeholder_slot_unlocked(at)) {
            const DWORD error = GetLastError();
            for (size_t j = 0; j < i; ++j)
                unmap_one_unlocked(static_cast<uint8_t*>(target) + j * page_bytes,
                                   *region, begin + j);
            SetLastError(error);
            return nullptr;
        }
        const auto handle = pages_.at(numbers[i]).section;
        if (host().map(handle, GetCurrentProcess(), at, 0, page_bytes,
                       MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) != at) {
            const DWORD error = GetLastError();
            for (size_t j = 0; j < i; ++j)
                unmap_one_unlocked(static_cast<uint8_t*>(target) + j * page_bytes,
                                   *region, begin + j);
            SetLastError(error);
            return nullptr;
        }
        region->mapped_pages[begin + i] = numbers[i];
        ++pages_.at(numbers[i]).aliases;
    }
    return target;
}

BOOL PhysicalMemory::map_scatter(void* const* destinations, size_t count,
                                  const uintptr_t* numbers) {
    if (!destinations || count == 0 || count > 65536 || !host().available()) {
        SetLastError(!host().available() ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    struct Mapping {
        void* destination;
        Region* region;
        size_t index;
        uintptr_t physical_frame;
    };
    std::lock_guard lock(mutex_);
    std::vector<Mapping> entries;
    std::unordered_set<uintptr_t> addresses;
    try {
        entries.reserve(count);
        addresses.reserve(count);
        // Validate every pointer, page number and destination before mapping
        // anything. Scatter elements need not belong to the same reservation.
        for (size_t i = 0; i < count; ++i) {
            void* destination = destinations[i];
            const uintptr_t value = reinterpret_cast<uintptr_t>(destination);
            if (!destination || !aligned(value) || !addresses.insert(value).second) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            Region* selected = nullptr;
            size_t index = 0;
            for (auto& [base, region] : regions_) {
                const uintptr_t first = reinterpret_cast<uintptr_t>(base);
                if (value < first) continue;
                const uintptr_t offset = value - first;
                if (offset / page_bytes < region.page_count) {
                    selected = &region;
                    index = static_cast<size_t>(offset / page_bytes);
                    break;
                }
            }
            if (!selected) {
                SetLastError(ERROR_INVALID_ADDRESS);
                return FALSE;
            }
            if (numbers && (selected->mapped_pages[index] != 0 ||
                            pages_.find(numbers[i]) == pages_.end())) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            entries.push_back({destination, selected, index,
                               numbers ? numbers[i] : uintptr_t{0}});
        }
    } catch (...) {
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }

    if (!numbers) {
        // Unmapping preserves each destination as a valid 64KiB placeholder.
        for (const auto& entry : entries) {
            if (!unmap_one_unlocked(entry.destination, *entry.region, entry.index))
                return FALSE;
        }
        return TRUE;
    }

    size_t mapped = 0;
    for (; mapped < entries.size(); ++mapped) {
        const auto& entry = entries[mapped];
        if (!split_placeholder_slot_unlocked(entry.destination)) {
            const DWORD error = GetLastError();
            for (size_t rollback = mapped; rollback > 0; --rollback) {
                const auto& done = entries[rollback - 1];
                unmap_one_unlocked(done.destination, *done.region, done.index);
            }
            SetLastError(error);
            return FALSE;
        }
        const HANDLE section = pages_.at(entry.physical_frame).section;
        const void* result = host().map(section, GetCurrentProcess(),
                                        entry.destination, 0, page_bytes,
                                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                        nullptr, 0);
        if (result != entry.destination) {
            const DWORD error = GetLastError();
            // The host mapping step failed. Unwind every completed mapping,
            // restoring the original placeholder state and reference counts.
            for (size_t rollback = mapped; rollback > 0; --rollback) {
                const auto& done = entries[rollback - 1];
                unmap_one_unlocked(done.destination, *done.region, done.index);
            }
            SetLastError(error);
            return FALSE;
        }
        entry.region->mapped_pages[entry.index] = entry.physical_frame;
        ++pages_.at(entry.physical_frame).aliases;
    }
    return TRUE;
}

bool PhysicalMemory::release_region_unlocked(void* address, Region& region) {
    // Only mapped pages need UnmapViewOfFile2; unsplit placeholders do not
    // create one allocation per page.
    for (size_t i = 0; i < region.page_count; ++i) {
        if (!region.mapped_pages[i]) continue;
        void* at = static_cast<uint8_t*>(address) + i * page_bytes;
        if (!unmap_one_unlocked(at, region, i)) return false;
    }

    uintptr_t cursor = reinterpret_cast<uintptr_t>(address);
    const uintptr_t region_bytes = region.page_count * page_bytes;
    if (region_bytes > std::numeric_limits<uintptr_t>::max() - cursor) {
        SetLastError(ERROR_ARITHMETIC_OVERFLOW);
        return false;
    }
    const uintptr_t end = cursor + region_bytes;
    // Each VirtualQuery allocation extent is one placeholder allocation.
    // Once a page has been mapped and unmapped, it is still a 64KiB
    // placeholder, so only touched pages create extra extents.
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_RESERVE ||
            reinterpret_cast<uintptr_t>(info.AllocationBase) != cursor ||
            info.RegionSize == 0 || info.RegionSize > end - cursor) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return false;
        }
        const uintptr_t next = cursor + info.RegionSize;
        if (!VirtualFree(reinterpret_cast<void*>(cursor), 0, MEM_RELEASE))
            return false;
        cursor = next;
    }
    return true;
}

BOOL PhysicalMemory::release_alias_region(void* address) {
    std::lock_guard lock(mutex_);
    auto it = regions_.find(address);
    if (it == regions_.end()) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return FALSE;
    }
    if (!release_region_unlocked(address, it->second)) return FALSE;
    regions_.erase(it);
    return TRUE;
}

bool PhysicalMemory::owns_alias_region_base(void* base) const {
    std::lock_guard lock(mutex_);
    return regions_.contains(base);
}

size_t PhysicalMemory::allocated_page_count() const {
    std::lock_guard lock(mutex_);
    return pages_.size();
}

PhysicalMemory::~PhysicalMemory() {
    // Clean up at process teardown, including unsplit large placeholders.
    for (auto& [base, region] : regions_)
        release_region_unlocked(base, region);
    for (auto& [frame, page] : pages_)
        if (page.section) CloseHandle(page.section);
}
} // namespace scarlett::memory
