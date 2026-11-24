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
    // Splitting into 64KiB placeholder segments lets each physical page be
    // independently mapped without any release/re-reserve race window.
    size_t segments = 1;
    for (size_t i = 1; i < count; ++i) {
        void* next = static_cast<uint8_t*>(address) + (i - 1) * page_bytes;
        if (!VirtualFree(next, page_bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            break;
        ++segments;
    }
    if (segments != count) {
        const DWORD error = GetLastError();
        // Every segment is a unique allocation after a split, so release all.
        for (size_t i = 0; i < segments; ++i) {
            const size_t offset = i * page_bytes;
            VirtualFree(static_cast<uint8_t*>(address) + offset, 0, MEM_RELEASE);
        }
        SetLastError(error);
        return nullptr;
    }
    try {
        Region region;
        region.page_count = count;
        region.mapped_pages.resize(count);
        std::lock_guard lock(mutex_);
        const auto [it, added] = regions_.emplace(address, std::move(region));
        if (!added) {
            for (size_t i = 0; i < count; ++i)
                VirtualFree(static_cast<uint8_t*>(address) + i * page_bytes, 0, MEM_RELEASE);
            SetLastError(ERROR_ALREADY_EXISTS);
            return nullptr;
        }
    } catch (...) {
        for (size_t i = 0; i < count; ++i)
            VirtualFree(static_cast<uint8_t*>(address) + i * page_bytes, 0, MEM_RELEASE);
        SetLastError(ERROR_OUTOFMEMORY);
        return nullptr;
    }
    return address;
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

BOOL PhysicalMemory::release_alias_region(void* address) {
    std::lock_guard lock(mutex_);
    auto it = regions_.find(address);
    if (it == regions_.end()) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return FALSE;
    }
    Region& region = it->second;
    for (size_t i = 0; i < region.page_count; ++i) {
        void* at = static_cast<uint8_t*>(address) + i * page_bytes;
        if (!unmap_one_unlocked(at, region, i))
            return FALSE;
    }
    for (size_t i = 0; i < region.page_count; ++i) {
        void* at = static_cast<uint8_t*>(address) + i * page_bytes;
        if (!VirtualFree(at, 0, MEM_RELEASE))
            return FALSE;
    }
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
    // Clean up even when a title terminates before freeing its pages.
    for (auto& [base, region] : regions_) {
        for (size_t i = 0; i < region.page_count; ++i) {
            void* at = static_cast<uint8_t*>(base) + i * page_bytes;
            if (region.mapped_pages[i] && host().available())
                host().unmap(GetCurrentProcess(), at, MEM_PRESERVE_PLACEHOLDER);
            VirtualFree(at, 0, MEM_RELEASE);
        }
    }
    for (auto& [frame, page] : pages_)
        if (page.section) CloseHandle(page.section);
}
} // namespace scarlett::memory
