#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

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
// Generated host-only fixture, never calls or loads any title executable.
bool verify_xmem_tools_capability_fixture() noexcept;
} // namespace scarlett::xmem
