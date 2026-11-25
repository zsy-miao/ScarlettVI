#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstddef>

namespace scarlett::xmem {

inline constexpr uint64_t kHostTransferGranularity = 2ull * 1024 * 1024;
inline constexpr uint64_t kHostInitialTitleBudget = 16ull * 1024 * 1024 * 1024;
inline constexpr uint32_t kHostTitleToSystemDirection = 1;

// Desktop budget ACCOUNTING ONLY. These are host policy values, not
// Xbox system-memory values, and do not physically relocate RAM.
struct HostXMemBudgetSnapshot {
    uint64_t title_limit = 0;
    uint64_t system_limit = 0;
    uint64_t title_heap_charged = 0;
};

// Verified title callsite passes EDX = integer-multiple-of-2MiB <=16MiB
// and ECX = 1, and checks EAX for a BOOL success. Other directions are
// not independently verified, and are rejected. This partial adapter
// only transfers host-owned XMemAlloc and XMemAllocDefault heap quota;
// title VM allocations, sparse physical pages, and device memory are
// not included in the accounting.
extern "C" BOOL WINAPI XMemTransferMemory(uint32_t direction,
                                            size_t bytes) noexcept;

HostXMemBudgetSnapshot host_xmem_budget_snapshot() noexcept;

// Internal admission/rollback used by the real XMemAlloc registry.
bool acquire_host_xmem_heap_budget(size_t bytes) noexcept;
void release_host_xmem_heap_budget(size_t bytes) noexcept;

// Testing only; MUST never be registered as an Xbox guest import.
// Reconfiguration is refused if any host XMemAlloc is still charged.
bool configure_host_xmem_budget_for_fixture(
    uint64_t title_limit, uint64_t system_limit) noexcept;
bool verify_xmem_transfer_fixture() noexcept;
} // namespace scarlett::xmem
