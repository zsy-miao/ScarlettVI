#pragma once
#include "dstorage_async_queue.h"
#include "dstorage_factory_host.h"
#include "dstorage_status_host.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace scarlett::storage {
struct HostDStorageFileX;

// Public Xbox GDK DSTORAGE_REQUEST layout; the EnqueueRequest METHOD SLOT
// is separately confirmed at guest +0x18 (0x1428C9A0B). The full layout
// follows public docs, not independently recovered for every title field.
// The queue may
// execute ONLY explicitly registered, generated-file fixture requests.
// All other guest pointers remain unread and yield E_NOTIMPL.
struct GuestDStorageRequest {
    uint64_t options = 0;
    void* destination = nullptr;
    uint32_t destination_size = 0;
    HostDStorageFileX* file = nullptr;
    uint64_t file_offset = 0;
    uint32_t source_size = 0;
    uint32_t intermediate_size = 0;
    uint64_t cancellation_tag = 0;
    const char* name = nullptr;
};
static_assert(sizeof(GuestDStorageRequest) == 64);
static_assert(offsetof(GuestDStorageRequest, destination) == 8);
static_assert(offsetof(GuestDStorageRequest, destination_size) == 16);
static_assert(offsetof(GuestDStorageRequest, file) == 24);
static_assert(offsetof(GuestDStorageRequest, file_offset) == 32);
static_assert(offsetof(GuestDStorageRequest, source_size) == 40);
static_assert(offsetof(GuestDStorageRequest, intermediate_size) == 44);
static_assert(offsetof(GuestDStorageRequest, cancellation_tag) == 48);
static_assert(offsetof(GuestDStorageRequest, name) == 56);

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
    virtual void STDMETHODCALLTYPE EnqueueRequest(
        const GuestDStorageRequest* request) noexcept = 0;
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

// Internal fixture-only hooks. These are NEVER registered as guest exports.
// They do not accept arbitrary title-owned buffers or dereference an
// unregistered raw guest request pointer.
HRESULT enqueue_host_dstorage_queue_fixture_read(
    HostDStorageQueueX2* queue, const HostFileReadRequest& request) noexcept;
// HOST-ONLY: read through a validated ScarlettVI-owned IDStorageFileX
// using an independently reopened handle that survives source Close/Release.
HRESULT enqueue_host_dstorage_queue_fixture_file_read(
    HostDStorageQueueX2* queue, HostDStorageFileX* source,
    uint64_t token, uint64_t offset, uint32_t bytes) noexcept;
// Register an immutable request snapshot and explicitly OWNED destination
// for the next raw COM EnqueueRequest call. Only host-generated test buffers.
// Guest title calls cannot register arbitrary pointers or destination RAM.
HRESULT register_host_dstorage_fixture_request(
    const GuestDStorageRequest* request,
    const std::shared_ptr<std::vector<uint8_t>>& destination) noexcept;
// HOST-ONLY alternative: pin an actual 64KiB XMem CPU alias by its
// synthetic physical-frame identity before queueing. Never accepts an
// arbitrary process pointer or crosses a mapped 64KiB page boundary.
HRESULT register_host_dstorage_fixture_alias_request(
    const GuestDStorageRequest* request, uintptr_t physical_frame) noexcept;
// Explicitly approve contiguous virtual destinations crossing mapped 64KiB
// XMem pages. All frame identities must match before a permit is issued.
HRESULT register_host_dstorage_fixture_alias_pages_request(
    const GuestDStorageRequest* request,
    const uintptr_t* physical_frames, size_t frame_count) noexcept;
void unregister_host_dstorage_fixture_request(
    const GuestDStorageRequest* request) noexcept;
HRESULT wait_host_dstorage_queue_fixture_read(
    HostDStorageQueueX2* queue, uint64_t token, DWORD timeout_ms,
    HostFileReadCompletion& output) noexcept;
HRESULT last_host_dstorage_queue_error(
    HostDStorageQueueX2* queue) noexcept;
uint32_t live_host_dstorage_queues() noexcept;
bool verify_dstorage_queue_com_fixture() noexcept;

} // namespace scarlett::storage
