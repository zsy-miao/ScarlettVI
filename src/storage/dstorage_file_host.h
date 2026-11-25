#pragma once
#include "dstorage_factory_host.h"
#include <filesystem>

namespace scarlett::storage {

// Original host implementation of the documented IDStorageFileX shape.
// Close and GetHandle vtable order is provisional until independently
// verified from an original-title caller. This is NOT GDK code.
struct HostDStorageFileX : public IUnknown {
    virtual void STDMETHODCALLTYPE Close() noexcept = 0;
    // Caller owns the duplicated Win32 handle and must CloseHandle it.
    virtual HANDLE STDMETHODCALLTYPE GetHandle() noexcept = 0;
};

// A fixture-only, explicitly configured HOST trust root. Unconfigured
// guest OpenFile calls continue to fail closed with E_NOTIMPL. Only
// bounded, relative generated-file paths (up to eight components) are
// accepted after the root is set. Opened handle paths must resolve inside
// that root. No title directory or actual console volume is mounted.
HRESULT configure_dstorage_file_fixture_root(
    const std::filesystem::path& trusted_root) noexcept;
void clear_dstorage_file_fixture_root() noexcept;
// Internal snapshot for fixture-only queue construction; not an Xbox API.
HRESULT snapshot_dstorage_file_fixture_root(
    std::filesystem::path& output) noexcept;
HRESULT open_host_dstorage_file(
    const wchar_t* relative_path, REFIID iid, void** output) noexcept;
// HOST-ONLY: reopen a ScarlettVI-owned live file object for an independent
// synchronous read position. The caller owns and must CloseHandle the result.
// Unknown/closed COM objects fail without dereferencing untrusted pointers.
HRESULT reopen_owned_dstorage_file_for_read(
    HostDStorageFileX* candidate, HANDLE* output) noexcept;
uint32_t live_host_dstorage_files() noexcept;
bool verify_dstorage_file_fixture() noexcept;

} // namespace scarlett::storage
