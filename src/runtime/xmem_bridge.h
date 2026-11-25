#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include "xmem_transfer.h"

namespace scarlett::xmem {
// Entry points follow the Xbox x64 XMem allocation ABI, based on publicly
// published function signatures. Graphics allocations are CPU-backed only;
// GPU-visible unified virtual address mapping is not yet supported.
extern "C" {
void* WINAPI XMemAlloc(size_t size, uint64_t attributes);
void* WINAPI XMemAllocDefault(size_t size, uint64_t attributes);
// Desktop-only capability policy: ScarlettVI does not expose an Xbox tools
// memory partition. Report FALSE; do not allocate/move memory as a side
// effect. This is not a verified Xbox developer-kit memory-service model.
BOOL WINAPI XMemIsToolsMemAvailable(void);
// Fail-closed host capability query, not a memory budget emulation.
// Xbox callers have been observed passing (selector, output pointer),
// but the output buffer size/layout varies by GDK version. We must not
// overwrite caller-owned memory with an invented console structure.
BOOL WINAPI XMemGetWorkingSetStatistics(uint32_t selector, void* output);
void WINAPI XMemFree(void* ptr, uint64_t attributes);
void WINAPI XMemFreeDefault(void* ptr, uint64_t attributes);
void* WINAPI XMemVirtualAlloc(void* address, size_t bytes,
                              DWORD allocation_type, uint64_t attributes, DWORD protection);
BOOL WINAPI XMemAllocatePhysicalPages(uintptr_t flags, uintptr_t* inout_count,
                                      uintptr_t* page_numbers);
BOOL WINAPI XMemFreePhysicalPages(uintptr_t count, const uintptr_t* page_numbers);
void* WINAPI XMemMapPhysicalPages(void* requested_address, uintptr_t page_count,
                                  const uintptr_t* page_numbers);
// Xbox x64 call pattern independently observed in the supplied title:
// RCX=address vector, RDX=count, R8=page frame vector (null to unmap);
// returns nonzero on success. Console-specific flags remain unsupported.
BOOL WINAPI XMemMapPhysicalPagesScatter(void* const* addresses,
                                        uintptr_t count, const uintptr_t* page_numbers);
// Only redirected into the mapped title's Windows IAT; the host application
// continues to use ordinary Windows VirtualFree directly.
BOOL WINAPI VirtualFreeBridge(void* base, size_t size, DWORD flags);
}
FARPROC find_export(std::string_view name) noexcept;
// Host-only validated snapshots of live XMemAlloc / XMemAllocDefault
// allocations, not arbitrary host memory or the separate XMemVirtualAlloc
// reservations. The registry lock covers validation AND memcpy, so a
// concurrent XMemFree cannot release the source halfway through copying.
// Unregistered, freed, overflowed or out-of-bounds ranges fail closed.
// The destination is unchanged on failure; bytes capped to 1 MiB.
bool snapshot_owned_xmem_allocation(
    const void* source, size_t bytes, void* host_output) noexcept;
// Copies a NUL-terminated UTF-16 path (<= max_chars) from a *single*
// currently live host-owned heap allocation. Does not read beyond the
// allocation even when the requested string is unterminated. The output
// is unchanged on failure. Both helpers are not guest DLL exports.
bool snapshot_owned_xmem_utf16(
    const wchar_t* source, size_t max_chars, std::wstring& output) noexcept;
// Lifetime pin for an exact writable subspan of a registered XMemAlloc
// block. A pinned allocation refuses XMemFree with ERROR_BUSY. This
// type never owns a guest pointer unless admitted by the registry.
class HostXMemWritePin final {
public:
    ~HostXMemWritePin() noexcept;
    HostXMemWritePin(const HostXMemWritePin&) = delete;
    HostXMemWritePin& operator=(const HostXMemWritePin&) = delete;
    size_t size() const noexcept { return bytes_; }
    void* address() const noexcept { return address_; }
    HRESULT write(const uint8_t* source, size_t bytes) noexcept;
private:
    friend HRESULT pin_owned_xmem_destination(
        void*, size_t, std::shared_ptr<HostXMemWritePin>&) noexcept;
    HostXMemWritePin(void* base, void* address, size_t bytes) noexcept
        : allocation_base_(base), address_(address), bytes_(bytes) {}
    void* allocation_base_ = nullptr;
    void* address_ = nullptr;
    size_t bytes_ = 0;
};
HRESULT pin_owned_xmem_destination(
    void* destination, size_t bytes,
    std::shared_ptr<HostXMemWritePin>& output) noexcept;
size_t active_xmem_write_pin_count() noexcept;
bool verify_xmem_heap_snapshots_fixture() noexcept;
// Generated host-only fixture, never calls or loads any title executable.
bool verify_xmem_tools_capability_fixture() noexcept;
bool verify_xmem_stats_failure_fixture() noexcept;
// Host budget accounting (NOT Xbox physical partition migration).
bool verify_xmem_transfer_fixture() noexcept;
} // namespace scarlett::xmem
