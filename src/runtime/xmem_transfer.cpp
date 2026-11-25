#include "xmem_transfer.h"
#include <limits>
#include <mutex>

namespace scarlett::xmem {
namespace {
class HostXMemBudget final {
public:
    static HostXMemBudget& instance() noexcept {
        static HostXMemBudget model;
        return model;
    }
    HostXMemBudgetSnapshot snapshot() noexcept {
        std::lock_guard lock(mutex_);
        return {title_limit_, system_limit_, charged_};
    }

    bool admit(size_t bytes) noexcept {
        std::lock_guard lock(mutex_);
        if (charged_ > title_limit_ ||
            static_cast<uint64_t>(bytes) > title_limit_ - charged_)
            return false;
        charged_ += bytes;
        return true;
    }
    void release(size_t bytes) noexcept {
        std::lock_guard lock(mutex_);
        // Only release memory that was admitted. No underflow on
        // unrecognized frees or failed host allocations.
        if (static_cast<uint64_t>(bytes) <= charged_)
            charged_ -= bytes;
    }
    BOOL transfer(uint32_t direction, size_t bytes) noexcept {
        if (direction != kHostTitleToSystemDirection) {
            SetLastError(ERROR_NOT_SUPPORTED);
            return FALSE;
        }
        if (static_cast<uint64_t>(bytes) % kHostTransferGranularity != 0) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        const std::lock_guard lock(mutex_);
        if (charged_ > title_limit_) {
            SetLastError(ERROR_INVALID_DATA);
            return FALSE;
        }
        // A zero transfer is an exact zero-delta, verified no-op; it
        // leaves allocation accounting and host Win32 errors unchanged.
        if (static_cast<uint64_t>(bytes) > title_limit_ - charged_) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
        if (static_cast<uint64_t>(bytes) >
            std::numeric_limits<uint64_t>::max() - system_limit_) {
            SetLastError(ERROR_ARITHMETIC_OVERFLOW);
            return FALSE;
        }
        title_limit_ -= bytes;
        system_limit_ += bytes;
        return TRUE;
    }

    bool configure_for_fixture(uint64_t title, uint64_t system) noexcept {
        std::lock_guard lock(mutex_);
        if (charged_ != 0 ||
            title > std::numeric_limits<uint64_t>::max() - system)
            return false;
        title_limit_ = title;
        system_limit_ = system;
        return true;
    }
private:
    std::mutex mutex_;
    uint64_t title_limit_ = kHostInitialTitleBudget;
    uint64_t system_limit_ = 0;
    uint64_t charged_ = 0;
};
} // namespace

extern "C" BOOL WINAPI XMemTransferMemory(
    uint32_t direction, size_t bytes) noexcept {
    return HostXMemBudget::instance().transfer(direction, bytes);
}
HostXMemBudgetSnapshot host_xmem_budget_snapshot() noexcept {
    return HostXMemBudget::instance().snapshot();
}
bool acquire_host_xmem_heap_budget(size_t bytes) noexcept {
    return HostXMemBudget::instance().admit(bytes);
}
void release_host_xmem_heap_budget(size_t bytes) noexcept {
    HostXMemBudget::instance().release(bytes);
}
bool configure_host_xmem_budget_for_fixture(
    uint64_t title, uint64_t system) noexcept {
    return HostXMemBudget::instance().configure_for_fixture(title, system);
}
} // namespace scarlett::xmem
