#pragma once
#include <Windows.h>
#include <Unknwn.h>
#include <cstdint>

namespace scarlett::storage {

// IID read as data from the original, unexecuted title at VA 0x1462FC7E8.
// This is an original MS-x64 COM ABI declaration, not an Xbox SDK header.
inline constexpr GUID kObservedXboxDStorageStatusIid{
    0xcd66d752, 0x2786, 0x44f7,
    {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};

// Xbox methods are GetHResult and IsComplete. The vtable order below
// follows the separately documented Windows DirectStorage ABI:
// IsComplete (+0x18), GetHResult (+0x20). Actual Xbox method callsites
// are not yet confirmed; do NOT present this as fully proven Xbox ABI.
struct HostDStorageStatusArrayX : public IUnknown {
    virtual bool STDMETHODCALLTYPE IsComplete(uint32_t index)
        noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetHResult(uint32_t index)
        noexcept = 0;
};

// Factory's independently verified CreateStatusArray signature.
// Name is a debug label and is deliberately not read or retained.
HRESULT create_host_dstorage_status_array(
    uint32_t capacity, const char* name,
    REFIID iid, void** output) noexcept;

// INTERNAL host worker hooks; never registered as Xbox exports. Only
// real committed worker completions (or explicit queue cancellation)
// may drive a status from E_PENDING to the operation's actual HRESULT.
HRESULT begin_host_dstorage_status(
    HostDStorageStatusArrayX* object, uint32_t index) noexcept;
HRESULT finish_host_dstorage_status(
    HostDStorageStatusArrayX* object, uint32_t index,
    HRESULT result) noexcept;

// Safe producer-side ownership check: returns an owned AddRef only for a
// still-live status object allocated by ScarlettVI. Caller must Release.
bool retain_owned_dstorage_status_array(
    HostDStorageStatusArrayX* candidate) noexcept;
uint32_t live_host_dstorage_status_arrays() noexcept;
bool verify_dstorage_status_array_fixture() noexcept;

} // namespace scarlett::storage
