#include "xmem_bridge.h"
#include <Windows.h>
#include <array>
#include <string_view>
#include <type_traits>

namespace scarlett::xmem {
namespace {
using ToolsQueryFn = BOOL (WINAPI*)(void);
static_assert(std::is_same_v<decltype(&XMemIsToolsMemAvailable), ToolsQueryFn>);
} // namespace

bool verify_xmem_tools_capability_fixture() noexcept {
    const auto export_ptr = find_export("XMemIsToolsMemAvailable");
    if (!export_ptr ||
        export_ptr != reinterpret_cast<FARPROC>(&XMemIsToolsMemAvailable))
        return false;

    // Exercise the same function-pointer shape as an IAT binding. On a
    // desktop without an Xbox tools-memory partition, the truthful
    // advertised capability is unavailable, consistently, with no Win32
    // thread-local error-code side effects.
    const auto query = reinterpret_cast<ToolsQueryFn>(export_ptr);
    const DWORD sentinel = 0x5134abcd;
    SetLastError(sentinel);
    for (unsigned i = 0; i < 256; ++i) {
        if (query() != FALSE || GetLastError() != sentinel)
            return false;
    }

    // The router must continue refusing unknown and higher-risk exports.
    for (const std::string_view missing : std::array{
             std::string_view{"XMemGetWorkingSetStatistics"},
             std::string_view{"XMemTransferMemory"},
             std::string_view{"XMemIsToolsMemAvailableByOrdinal"},
             std::string_view{"xmemistoolsmemavailable"},
             std::string_view{""}}) {
        if (find_export(missing) != nullptr)
            return false;
    }
    // Existing supported exports cannot disappear as a side effect.
    for (const std::string_view present : std::array{
             std::string_view{"XMemAlloc"},
             std::string_view{"XMemVirtualAlloc"},
             std::string_view{"XMemVirtualQuery"},
             std::string_view{"XMemMapPhysicalPagesScatter"}}) {
        if (!find_export(present))
            return false;
    }
    SetLastError(ERROR_SUCCESS);
    return true;
}
} // namespace scarlett::xmem
