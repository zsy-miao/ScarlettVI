#pragma once
#include <Windows.h>
#include <Unknwn.h>
#include <cstdint>
#include <string_view>

namespace scarlett::storage {

// IID independently read from the original title's data-only reference
// at VA 0x1462FC7D8. Microsoft documents Xbox DirectStorage factory
// as IDStorageFactoryX2; only this observed IID and IUnknown are exposed.
// This header is an ORIGINAL ABI-shaped host declaration, NOT GDK code.
inline constexpr GUID kObservedXboxDStorageFactoryIid{
    0x62959d11, 0xd45f, 0x4884,
    {0xa0,0xa3,0xf7,0x86,0x2e,0xa2,0x43,0xae}};
inline constexpr GUID kObservedXboxDStorageFileIid{
    0x43704f9f, 0x27c6, 0x4a9f,
    {0x88,0x15,0xd1,0x65,0x27,0x89,0x4d,0xf4}};
inline constexpr GUID kObservedXboxDStorageQueueIid{
    0x3c02cb0c, 0xc050, 0x4195,
    {0x99,0xa1,0xa1,0xce,0x94,0xba,0x33,0xc5}};
inline constexpr uint32_t kDefaultStagingBytes = 32u * 1024 * 1024;

// Verified game CALLSITE order (IUnknown + IDStorageFactoryX 6,
// IDStorageFactoryX1 GetStats, IDStorageFactoryX2 CreateQueue1).
// Unknown queue/file descriptor layouts are NEVER read by this class.
struct HostDStorageFactoryX2 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateQueue(
        const void* desc, REFIID iid, void** output) noexcept = 0;
    // Original title independently verifies OpenFile at +0x20
    // (path, REFIID, out) and CreateStatusArray at +0x28
    // (capacity, debug name, REFIID, out), NOT alphabetical order.
    virtual HRESULT STDMETHODCALLTYPE OpenFile(
        const wchar_t* path, REFIID iid, void** output) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateStatusArray(
        uint32_t capacity, const char* name, REFIID iid, void** output) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCpuAffinity(uint64_t affinity) noexcept = 0;
    virtual void STDMETHODCALLTYPE SetDebugFlags(uint32_t flags) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE SetStagingBufferSize(uint32_t bytes) noexcept = 0;
    virtual void STDMETHODCALLTYPE GetStats(void* unknown_sized) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateQueue1(
        const void* desc, REFIID iid, void** output) noexcept = 0;
};
static_assert(sizeof(HostDStorageFactoryX2*) == sizeof(void*));

extern "C" HRESULT WINAPI DStorageGetFactory(
    REFIID iid, void** output) noexcept;

FARPROC find_dstorage_export(std::string_view name) noexcept;
// HOST-ONLY diagnostic helpers, not Xbox DLL guest exports.
uint32_t host_dstorage_staging_bytes() noexcept;
// Internal-only lease for a host-owned asynchronous file queue. It prevents
// staging resize while the queue exists; never exported to Xbox guest code.
bool acquire_host_dstorage_queue_lease() noexcept;
void release_host_dstorage_queue_lease() noexcept;
uint32_t active_host_dstorage_queue_leases() noexcept;
// Independent guest-file lifetime leases block staging reconfiguration.
bool acquire_host_dstorage_file_lease() noexcept;
void release_host_dstorage_file_lease() noexcept;
uint32_t active_host_dstorage_file_leases() noexcept;
bool verify_dstorage_staging_pages_for_fixture() noexcept;
bool verify_dstorage_factory_fixture() noexcept;

} // namespace scarlett::storage
