#include "xmem_transfer.h"
#include "xmem_bridge.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

namespace scarlett::xmem {
namespace {
using TransferFn = BOOL (WINAPI*)(uint32_t, size_t) noexcept;
static_assert(std::is_same_v<decltype(&XMemTransferMemory), TransferFn>);

constexpr size_t kMiB = 1024 * 1024;
constexpr size_t kStep = static_cast<size_t>(kHostTransferGranularity);
bool equal(const HostXMemBudgetSnapshot& a,
           const HostXMemBudgetSnapshot& b) noexcept {
    return a.title_limit == b.title_limit &&
           a.system_limit == b.system_limit &&
           a.title_heap_charged == b.title_heap_charged;
}

struct BudgetRestore {
    HostXMemBudgetSnapshot initial;
    ~BudgetRestore() {
        (void)configure_host_xmem_budget_for_fixture(
            initial.title_limit, initial.system_limit);
    }
};
struct AllocationGuard {
    void* pointer = nullptr;
    ~AllocationGuard() { if (pointer) XMemFree(pointer, 0); }
    void reset() {
        if (pointer) XMemFree(pointer, 0);
        pointer = nullptr;
    }
};
} // namespace

bool verify_xmem_transfer_fixture() noexcept {
    try {
        const FARPROC export_ptr = find_export("XMemTransferMemory");
        if (export_ptr != reinterpret_cast<FARPROC>(&XMemTransferMemory) ||
            find_export("XMemTransferMemoryOrdinal") ||
            find_export("xmemtransfermemory"))
            return false;
        const auto transfer = reinterpret_cast<TransferFn>(export_ptr);
        const HostXMemBudgetSnapshot before = host_xmem_budget_snapshot();
        if (before.title_heap_charged != 0)
            return false;
        BudgetRestore restore{before};
        if (!configure_host_xmem_budget_for_fixture(6 * kMiB, 4 * kMiB))
            return false;

        const auto initial = host_xmem_budget_snapshot();
        if (initial.title_limit != 6 * kMiB ||
            initial.system_limit != 4 * kMiB ||
            initial.title_heap_charged)
            return false;
        SetLastError(0x55667788);
        if (transfer(1, 0) != TRUE ||
            GetLastError() != 0x55667788 ||
            !equal(host_xmem_budget_snapshot(), initial))
            return false;

        for (uint32_t invalid_direction : std::array<uint32_t, 3>{0,2,0xffffffffu}) {
            SetLastError(ERROR_SUCCESS);
            if (transfer(invalid_direction, kStep) != FALSE ||
                GetLastError() != ERROR_NOT_SUPPORTED ||
                !equal(host_xmem_budget_snapshot(), initial))
                return false;
        }
        for (size_t invalid_bytes : std::array<size_t, 4>{1, kMiB,
                                                           kStep - 1,
                                                           kStep + 1}) {
            SetLastError(ERROR_SUCCESS);
            if (transfer(1, invalid_bytes) != FALSE ||
                GetLastError() != ERROR_INVALID_PARAMETER ||
                !equal(host_xmem_budget_snapshot(), initial))
                return false;
        }
        SetLastError(ERROR_SUCCESS);
        if (transfer(1, 8 * kMiB) != FALSE ||
            GetLastError() != ERROR_NOT_ENOUGH_MEMORY ||
            !equal(host_xmem_budget_snapshot(), initial))
            return false;

        // XMemAlloc actually consumes title-heap quota. A full-budget
        // transfer is rejected while allocations are live, with no
        // mutation of the title/system partitions.
        AllocationGuard first{XMemAlloc(3 * kMiB, 0)};
        if (!first.pointer ||
            host_xmem_budget_snapshot().title_heap_charged != 3 * kMiB)
            return false;
        SetLastError(ERROR_SUCCESS);
        if (transfer(1, 4 * kMiB) != FALSE ||
            GetLastError() != ERROR_NOT_ENOUGH_MEMORY)
            return false;
        if (transfer(1, kStep) != TRUE)
            return false;
        const auto transferred = host_xmem_budget_snapshot();
        if (transferred.title_limit != 4 * kMiB ||
            transferred.system_limit != 6 * kMiB ||
            transferred.title_heap_charged != 3 * kMiB)
            return false;

        // The transferred budget is enforced in subsequent real host
        // allocations. Failure happens before a large malloc is tried.
        AllocationGuard rejected{XMemAllocDefault(2 * kMiB, 0)};
        if (rejected.pointer || GetLastError() != ERROR_NOT_ENOUGH_MEMORY)
            return false;
        AllocationGuard second{XMemAllocDefault(kMiB, 0)};
        if (!second.pointer)
            return false;
        if (host_xmem_budget_snapshot().title_heap_charged != 4 * kMiB)
            return false;
        SetLastError(ERROR_SUCCESS);
        if (transfer(1, kStep) != FALSE ||
            GetLastError() != ERROR_NOT_ENOUGH_MEMORY)
            return false;

        // Real Windows-backed allocations remain usable and retain
        // ownership even after title's free quota shrinks.
        auto* ptr1 = static_cast<uint8_t*>(first.pointer);
        auto* ptr2 = static_cast<uint8_t*>(second.pointer);
        ptr1[0] = 0xa5;
        ptr1[3 * kMiB - 1] = 0x5a;
        ptr2[0] = 0x13;
        ptr2[kMiB - 1] = 0x37;
        if (ptr1[0] != 0xa5 || ptr1[3 * kMiB - 1] != 0x5a ||
            ptr2[0] != 0x13 || ptr2[kMiB - 1] != 0x37)
            return false;

        first.reset();
        const auto after_free = host_xmem_budget_snapshot();
        if (after_free.title_heap_charged != kMiB ||
            transfer(1, kStep) != TRUE)
            return false;
        const auto after_second_transfer = host_xmem_budget_snapshot();
        if (after_second_transfer.title_limit != 2 * kMiB ||
            after_second_transfer.system_limit != 8 * kMiB ||
            after_second_transfer.title_heap_charged != kMiB)
            return false;
        AllocationGuard third{XMemAlloc(kMiB, 0)};
        if (!third.pointer ||
            host_xmem_budget_snapshot().title_heap_charged != 2 * kMiB)
            return false;
        AllocationGuard fourth{XMemAlloc(1, 0)};
        if (fourth.pointer || GetLastError() != ERROR_NOT_ENOUGH_MEMORY)
            return false;
        second.reset();
        third.reset();
        if (host_xmem_budget_snapshot().title_heap_charged != 0)
            return false;

        // Four concurrent requests run against a shared 10 MiB title
        // quota. Exactly five 2-MiB transfers can succeed; no double
        // spend and the total remains constant.
        if (!configure_host_xmem_budget_for_fixture(10 * kMiB, 0))
            return false;
        std::atomic<int> successes{0};
        std::atomic<bool> correct_failures{true};
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 4; ++i) {
                    SetLastError(ERROR_SUCCESS);
                    const BOOL result = transfer(1, kStep);
                    if (result == TRUE)
                        successes.fetch_add(1);
                    else if (GetLastError() != ERROR_NOT_ENOUGH_MEMORY)
                        correct_failures.store(false);
                }
            });
        }
        for (auto& thread : threads)
            thread.join();
        const auto concurrent = host_xmem_budget_snapshot();
        if (!correct_failures.load() || successes.load() != 5 ||
            concurrent.title_limit != 0 ||
            concurrent.system_limit != 10 * kMiB ||
            concurrent.title_heap_charged != 0)
            return false;
        AllocationGuard exhausted{XMemAlloc(1, 0)};
        if (exhausted.pointer || GetLastError() != ERROR_NOT_ENOUGH_MEMORY)
            return false;

        if (!configure_host_xmem_budget_for_fixture(
                before.title_limit, before.system_limit))
            return false;
        return equal(host_xmem_budget_snapshot(), before);
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::xmem
