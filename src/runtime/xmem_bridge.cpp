#include "xmem_bridge.h"
#include "xmem_transfer.h"
#include "physical_memory.h"
#include "xmem_query.h"
#include <malloc.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>

namespace scarlett::xmem {
namespace {
enum class AllocationKind { AlignedHeap, ReservedPages };
struct Allocation {
    AllocationKind kind;
    size_t bytes;
    uint64_t attributes;
    bool budget_charged = false;
};
// One independent allocation registry for every title in the process.
// Entries remain in the registry until explicitly released. This allows us
// to reject unsupported frees without corrupting unrelated host allocations.
class Registry final {
public:
    static Registry& get() {
        static Registry instance;
        return instance;
    }
    void* alloc(size_t requested, uint64_t attrs) noexcept {
        const unsigned alignment_log2 = static_cast<unsigned>((attrs >> 24u) & 31u);
        const unsigned memory_type = static_cast<unsigned>((attrs >> 29u) & 15u);
        const unsigned page_type = static_cast<unsigned>((attrs >> 14u) & 3u);
        // Physical and >64KB page allocations require a console page backend.
        if (memory_type >= 7 || page_type >= 2 || alignment_log2 > 16) {
            SetLastError(ERROR_NOT_SUPPORTED);
            return nullptr;
        }
        const size_t size = std::max<size_t>(requested, 1);
        // Reserve budget BEFORE allocating physical host heap memory.
        // TransferMemory cannot shrink the admitted title quota below
        // already-admitted XMemAlloc usage, even under concurrent calls.
        if (!acquire_host_xmem_heap_budget(size)) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        const size_t alignment = size_t(1) << std::max(4u, alignment_log2);
        const bool use_pages = memory_type != 0 || alignment > 4096 || page_type == 1;
        void* pointer = nullptr;
        if (use_pages) {
            pointer = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (pointer && reinterpret_cast<uintptr_t>(pointer) % alignment) {
                VirtualFree(pointer, 0, MEM_RELEASE);
                pointer = nullptr;
            }
        } else {
            pointer = _aligned_malloc(size, alignment);
            if (pointer) memset(pointer, 0, size);
        }
        if (!pointer) {
            release_host_xmem_heap_budget(size);
            SetLastError(ERROR_OUTOFMEMORY);
            return nullptr;
        }
        try {
            const std::lock_guard lock(mutex_);
            allocations_.emplace(pointer, Allocation{use_pages ? AllocationKind::ReservedPages
                                                              : AllocationKind::AlignedHeap,
                                                      size, attrs, true});
        } catch (...) {
            if (use_pages) VirtualFree(pointer, 0, MEM_RELEASE);
            else _aligned_free(pointer);
            release_host_xmem_heap_budget(size);
            SetLastError(ERROR_OUTOFMEMORY);
            return nullptr;
        }
        return pointer;
    }

    void free(void* ptr, uint64_t) noexcept {
        if (!ptr) return;
        Allocation allocation{};
        {
            const std::lock_guard lock(mutex_);
            const auto found = allocations_.find(ptr);
            if (found == allocations_.end()) {
                SetLastError(ERROR_INVALID_ADDRESS);
                return;
            }
            allocation = found->second;
            allocations_.erase(found);
        }
        if (allocation.kind == AllocationKind::AlignedHeap) _aligned_free(ptr);
        else VirtualFree(ptr, 0, MEM_RELEASE);
        if (allocation.budget_charged)
            release_host_xmem_heap_budget(allocation.bytes);
    }

    void* reserve(void* address, size_t size, DWORD allocation_type,
                  uint64_t attributes, DWORD protection) noexcept {
        if (size == 0) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        // Xbox's 64KiB virtual physical mappings are backed by Windows
        // placeholder ranges, not committed private CPU memory.
        if ((allocation_type & MEM_64K_PAGES) == MEM_64K_PAGES) {
            if (allocation_type != (MEM_64K_PAGES | MEM_RESERVE)) {
                SetLastError(ERROR_NOT_SUPPORTED);
                return nullptr;
            }
            return memory::PhysicalMemory::instance().reserve_alias_region(address, size);
        }
        const DWORD accepted_flags = MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN;
        const DWORD type = allocation_type & accepted_flags;
        if ((allocation_type & ~accepted_flags) != 0 ||
            (type & (MEM_COMMIT | MEM_RESERVE)) == 0 ||
            ((type & MEM_TOP_DOWN) && ((type & MEM_RESERVE) == 0))) {
            SetLastError(ERROR_NOT_SUPPORTED);
            return nullptr;
        }
        const unsigned page_type = static_cast<unsigned>((attributes >> 14u) & 3u);
        const unsigned memory_type = static_cast<unsigned>((attributes >> 29u) & 15u);
        if (page_type >= 2 || memory_type >= 7) {
            SetLastError(ERROR_NOT_SUPPORTED);
            return nullptr;
        }
        // Do not reinterpret console-specific GPU protection flags as
        // equivalent desktop CPU flags. They need dedicated graphics HLE.
        switch (protection) {
        case PAGE_READWRITE:
        case PAGE_READONLY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_NOACCESS: break;
        default:
            SetLastError(ERROR_NOT_SUPPORTED);
            return nullptr;
        }
        void* pointer = VirtualAlloc(address, size, type, protection);
        if (!pointer) return nullptr;
        if ((type & MEM_RESERVE) != 0) {
            try {
                const std::lock_guard lock(mutex_);
                allocations_.emplace(pointer, Allocation{AllocationKind::ReservedPages, size, attributes});
            } catch (...) {
                VirtualFree(pointer, 0, MEM_RELEASE);
                SetLastError(ERROR_OUTOFMEMORY);
                return nullptr;
            }
        }
        return pointer;
    }

    ~Registry() {
        // The compatibility layer owns only allocations present in this map.
        for (const auto& [ptr, entry] : allocations_) {
            if (entry.kind == AllocationKind::AlignedHeap) _aligned_free(ptr);
            else VirtualFree(ptr, 0, MEM_RELEASE);
        }
    }
private:
    std::mutex mutex_;
    std::unordered_map<void*, Allocation> allocations_;
};
} // namespace

extern "C" void* WINAPI XMemAlloc(size_t size, uint64_t attrs) {
    return Registry::get().alloc(size, attrs);
}
extern "C" void* WINAPI XMemAllocDefault(size_t size, uint64_t attrs) {
    return Registry::get().alloc(size, attrs);
}
// No Xbox tools-memory partition or console device exists in ScarlettVI's
// desktop host. FALSE is an actual capability answer (not an assumed
// success result), and this query leaves Win32 thread error state intact.
extern "C" BOOL WINAPI XMemIsToolsMemAvailable(void) {
    return FALSE;
}
// The host has no Xbox working-set budget service. Unlike a fabricated
// success, this deterministic failure cannot overrun an unknown-version
// output structure or advertise graphics RAM we cannot back.
// This is an intentionally incomplete host error path.
extern "C" BOOL WINAPI XMemGetWorkingSetStatistics(
    uint32_t selector, void* output) {
    if (!output) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    (void)selector; // no Xbox memory selectors have verified host semantics
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
extern "C" void WINAPI XMemFree(void* ptr, uint64_t attrs) {
    Registry::get().free(ptr, attrs);
}
extern "C" void WINAPI XMemFreeDefault(void* ptr, uint64_t attrs) {
    Registry::get().free(ptr, attrs);
}
extern "C" void* WINAPI XMemVirtualAlloc(void* addr, size_t bytes,
                                        DWORD flags, uint64_t attrs, DWORD protect) {
    return Registry::get().reserve(addr, bytes, flags, attrs, protect);
}
extern "C" BOOL WINAPI VirtualFreeBridge(void* base, size_t size, DWORD flags) {
    auto& physical = memory::PhysicalMemory::instance();
    if (physical.owns_alias_region_base(base)) {
        if (size != 0 || flags != MEM_RELEASE) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        return physical.release_alias_region(base);
    }
    return ::VirtualFree(base, size, flags);
}
extern "C" BOOL WINAPI XMemAllocatePhysicalPages(uintptr_t flags, uintptr_t* count,
                                                 uintptr_t* numbers) {
    return memory::PhysicalMemory::instance().allocate_pages(flags, count, numbers);
}
extern "C" BOOL WINAPI XMemFreePhysicalPages(uintptr_t count, const uintptr_t* numbers) {
    return memory::PhysicalMemory::instance().free_pages(count, numbers);
}
extern "C" void* WINAPI XMemMapPhysicalPages(void* address, uintptr_t count,
                                             const uintptr_t* numbers) {
    return memory::PhysicalMemory::instance().map_pages(address, count, numbers);
}
extern "C" BOOL WINAPI XMemMapPhysicalPagesScatter(void* const* addresses,
                                                     uintptr_t count,
                                                     const uintptr_t* numbers) {
    return memory::PhysicalMemory::instance().map_scatter(
        addresses, static_cast<size_t>(count), numbers);
}
FARPROC find_export(std::string_view symbol) noexcept {
    if (symbol == "XMemTransferMemory")
        return reinterpret_cast<FARPROC>(&XMemTransferMemory);
    if (symbol == "XMemAlloc") return reinterpret_cast<FARPROC>(&XMemAlloc);
    if (symbol == "XMemAllocDefault") return reinterpret_cast<FARPROC>(&XMemAllocDefault);
    if (symbol == "XMemIsToolsMemAvailable")
        return reinterpret_cast<FARPROC>(&XMemIsToolsMemAvailable);
    if (symbol == "XMemGetWorkingSetStatistics")
        return reinterpret_cast<FARPROC>(&XMemGetWorkingSetStatistics);
    if (symbol == "XMemFree") return reinterpret_cast<FARPROC>(&XMemFree);
    if (symbol == "XMemFreeDefault") return reinterpret_cast<FARPROC>(&XMemFreeDefault);
    if (symbol == "XMemVirtualAlloc") return reinterpret_cast<FARPROC>(&XMemVirtualAlloc);
    if (symbol == "XMemAllocatePhysicalPages") return reinterpret_cast<FARPROC>(&XMemAllocatePhysicalPages);
    if (symbol == "XMemFreePhysicalPages") return reinterpret_cast<FARPROC>(&XMemFreePhysicalPages);
    if (symbol == "XMemMapPhysicalPages") return reinterpret_cast<FARPROC>(&XMemMapPhysicalPages);
    if (symbol == "XMemMapPhysicalPagesScatter") return reinterpret_cast<FARPROC>(&XMemMapPhysicalPagesScatter);
    if (symbol == "XMemVirtualQuery") return reinterpret_cast<FARPROC>(&XMemVirtualQuery);
    return nullptr; // never mask an unknown API with a fake success stub
}
} // namespace scarlett::xmem
