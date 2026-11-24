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
void WINAPI XMemFree(void* ptr, uint64_t attributes);
void WINAPI XMemFreeDefault(void* ptr, uint64_t attributes);
void* WINAPI XMemVirtualAlloc(void* address, size_t bytes,
                              DWORD allocation_type, uint64_t attributes, DWORD protection);
}
FARPROC find_export(std::string_view name) noexcept;
} // namespace scarlett::xmem
