#include "xmem_query.h"
#include <cstring>

namespace scarlett::xmem {
namespace {
// The two least significant flags at +0x30 are a ScarlettVI *desktop*
// compatibility approximation, NOT confirmed Xbox memory flags.
// Bit 0 = CPU readable; bit 1 = CPU writable.
uint64_t desktop_access_flags(const MEMORY_BASIC_INFORMATION& info) noexcept {
    if (info.State != MEM_COMMIT ||
        (info.Protect & PAGE_GUARD) ||
        (info.Protect & PAGE_NOACCESS))
        return 0;
    const DWORD protect = info.Protect & 0xffu;
    switch (protect) {
    case PAGE_READONLY:
    case PAGE_EXECUTE_READ:
        return 0x01;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return 0x03;
    default:
        return 0;
    }
}
}
extern "C" BOOL WINAPI XMemVirtualQuery(HANDLE process, const void* address,
                                        VirtualQueryInfo* output) {
    if (!process || !address || !output) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    MEMORY_BASIC_INFORMATION query{};
    // VirtualQueryEx is used deliberately: the examined DLL passes an
    // explicit process handle, not an implicit current-process query.
    if (VirtualQueryEx(process, address, &query, sizeof(query)) != sizeof(query))
        return FALSE;

    VirtualQueryInfo result{};
    result.basic = query;
    result.desktop_access_flags = desktop_access_flags(query);
    *output = result;
    return TRUE;
}
} // namespace scarlett::xmem
