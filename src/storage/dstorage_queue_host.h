#pragma once
#include "dstorage_async_queue.h"
#include "dstorage_factory_host.h"
#include "dstorage_status_host.h"
#include <cstddef>
#include <cstdint>

namespace scarlett::storage {
struct HostDStorageFileX;

// Original host declaration of the published DirectStorage queue descriptor.
// The verified Microsoft public field order is source/capacity/priority/name/
// options, but Xbox-specific layout is still verified only for the 64-bit ABI.
struct GuestDStorageQueueDesc1 {
    uint64_t source_type;
    uint16_t capacity;
    int8_t priority;
    const char* name; // optional and deliberately never dereferenced
    uint64_t options;
};
static_assert(sizeof(GuestDStorageQueueDesc1) == 32);
static_assert(offsetof(GuestDStorageQueueDesc1, capacity) == 8);
static_assert(offsetof(GuestDStorageQueueDesc1, name) == 16);
static_assert(offsetof(GuestDStorageQueueDesc1, options) == 24);

// Provisional Xbox IDStorageQueueX2 method order, following the published
// DirectStorage queue family and Windows desktop vtable ordering. The game's
// own queue virtual callsites must still be checked before enabling titles.
struct HostDStorageQueueX2 : IUnknown {
    virtual void STDMETHODCALLTYPE EnqueueRequest(const void* request) noexcept = 0;
    virtual void STDMETHODCALLTYPE EnqueueStatus(
        HostDStorageStatusArrayX* array, uint32_t index) noexcept = 0;
    virtual void STDMETHODCALLTYPE EnqueueSignal(
        IUnknown* fence, uint64_t value) noexcept = 0;
    virtual void STDMETHODCALLTYPE Submit() noexcept = 0;
    virtual void STDMETHODCALLTYPE CancelRequestsWithTag(
        uint64_t mask, uint64_t value) noexcept = 0;
    virtual void STDMETHODCALLTYPE Close() noexcept = 0;
    virtual HANDLE STDMETHODCALLTYPE GetErrorEvent() noexcept = 0;
    virtual void STDMETHODCALLTYPE RetrieveErrorRecord(
        void* unverified_sized) noexcept = 0;
    virtual void STDMETHODCALLTYPE Query(void* unverified_sized) noexcept = 0;
    virtual void STDMETHODCALLTYPE EnqueueSetEvent(HANDLE event) noexcept = 0;
    virtual void STDMETHODCALLTYPE Query1(void* unverified_sized) noexcept = 0;
};

HRESULT create_host_dstorage_queue1(
    const void* desc, REFIID iid, void** output) noexcept;

// Internal fixture-only hooks. These are NEVER registered as guest exports,
// and do not interpret DSTORAGE_REQUEST or title-owned buffers.
HRESULT enqueue_host_dstorage_queue_fixture_read(
    HostDStorageQueueX2* queue, const HostFileReadRequest& request) noexcept;
// HOST-ONLY: read through a validated ScarlettVI-owned IDStorageFileX
// using an independently reopened handle that survives source Close/Release.
HRESULT enqueue_host_dstorage_queue_fixture_file_read(
    HostDStorageQueueX2* queue, HostDStorageFileX* source,
    uint64_t token, uint64_t offset, uint32_t bytes) noexcept;
HRESULT wait_host_dstorage_queue_fixture_read(
    HostDStorageQueueX2* queue, uint64_t token, DWORD timeout_ms,
    HostFileReadCompletion& output) noexcept;
HRESULT last_host_dstorage_queue_error(
    HostDStorageQueueX2* queue) noexcept;
uint32_t live_host_dstorage_queues() noexcept;
bool verify_dstorage_queue_com_fixture() noexcept;

} // namespace scarlett::storage
