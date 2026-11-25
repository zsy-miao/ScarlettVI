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
// guest OpenFile calls continue to fail closed with E_NOTIMPL. No title
// directory is mounted and no game assets are opened.
HRESULT configure_dstorage_file_fixture_root(
    const std::filesystem::path& trusted_root) noexcept;
void clear_dstorage_file_fixture_root() noexcept;
HRESULT open_host_dstorage_file(
    const wchar_t* leaf, REFIID iid, void** output) noexcept;
uint32_t live_host_dstorage_files() noexcept;
bool verify_dstorage_file_fixture() noexcept;

} // namespace scarlett::storage
