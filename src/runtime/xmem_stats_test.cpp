#include "xmem_bridge.h"
#include <Windows.h>
#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace scarlett::xmem {
namespace {
using QueryFn = BOOL (WINAPI*)(uint32_t, void*);
static_assert(std::is_same_v<decltype(&XMemGetWorkingSetStatistics), QueryFn>);
} // namespace

bool verify_xmem_stats_failure_fixture() noexcept {
    FARPROC address = find_export("XMemGetWorkingSetStatistics");
    if (!address ||
        address != reinterpret_cast<FARPROC>(&XMemGetWorkingSetStatistics))
        return false;
    const auto query = reinterpret_cast<QueryFn>(address);

    // The caller's real structure size is GDK-version-dependent.
    // Exercise small output regions and independently guarded bytes;
    // even a one-byte output must remain completely untouched.
    constexpr std::array<uint32_t, 4> selectors{
        0u, 0x20000001u, 0x20003000u, 0xffffffffu};
    for (const uint32_t selector : selectors) {
        for (size_t output_length = 1; output_length <= 64; ++output_length) {
            std::array<uint8_t, 96> buffer{};
            for (size_t i = 0; i < buffer.size(); ++i)
                buffer[i] = static_cast<uint8_t>(
                    (i * 37u + output_length * 11u + selector) & 0xffu);
            const auto before = buffer;
            SetLastError(0x5a5a5a5au);
            if (query(selector, buffer.data() + 16) != FALSE ||
                GetLastError() != ERROR_NOT_SUPPORTED || buffer != before)
                return false;
        }
    }

    // Bad arguments are a genuine error, not a fabricated success, and
    // must not access the pointer or expose a bogus memory budget.
    SetLastError(0x5a5a5a5au);
    if (query(0x20000001u, nullptr) != FALSE ||
        GetLastError() != ERROR_INVALID_PARAMETER)
        return false;

    // Host budget transfer is now bound with verified two-argument ABI;
    // the stats output layout remains unknown and must fail closed.
    if (find_export("XMemTransferMemory") !=
            reinterpret_cast<FARPROC>(&XMemTransferMemory) ||
        find_export("xmemgetworkingsetstatistics") != nullptr ||
        find_export("XMemGetWorkingSetStatistics_ordinal") != nullptr ||
        !find_export("XMemIsToolsMemAvailable") ||
        !find_export("XMemVirtualQuery"))
        return false;

    SetLastError(ERROR_SUCCESS);
    return true;
}
} // namespace scarlett::xmem
