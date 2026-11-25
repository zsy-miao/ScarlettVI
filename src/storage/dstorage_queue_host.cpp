#include "dstorage_queue_host.h"
#include "dstorage_alias_destination.h"
#include "dstorage_file_host.h"
#include <atomic>
#include <array>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scarlett::storage {
namespace {
std::atomic<uint32_t> live_queues{0};
struct RegisteredFixtureRequest {
    GuestDStorageRequest snapshot;
    std::shared_ptr<std::vector<uint8_t>> destination;
    std::shared_ptr<HostDStorageAliasDestination> alias;
};
std::mutex fixture_request_mutex;
std::unordered_map<const GuestDStorageRequest*, RegisteredFixtureRequest>
    fixture_requests;

class HostQueue final : public HostDStorageQueueX2 {
public:
    HostQueue(std::unique_ptr<HostAsyncFileQueue> worker,
              HANDLE error_event) noexcept
        : worker_(std::move(worker)), error_event_(error_event) {
        live_queues.fetch_add(1, std::memory_order_relaxed);
    }
    ~HostQueue() {
        if (worker_) {
            worker_->shutdown();
            worker_.reset(); // releases staging lease and held COM statuses
        }
        if (error_event_) CloseHandle(error_event_);
        live_queues.fetch_sub(1, std::memory_order_relaxed);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!IsEqualGUID(iid, IID_IUnknown) &&
            !IsEqualGUID(iid, kObservedXboxDStorageQueueIid))
            return E_NOINTERFACE;
        if (!AddRef()) return E_FAIL;
        *output = static_cast<HostDStorageQueueX2*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        ULONG count = references_.load(std::memory_order_relaxed);
        while (count && count != std::numeric_limits<ULONG>::max()) {
            if (references_.compare_exchange_weak(
                    count, count + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed))
                return count + 1;
        }
        return 0;
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG count = references_.load(std::memory_order_relaxed);
        while (count) {
            if (references_.compare_exchange_weak(
                    count, count - 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                if (count == 1) delete this;
                return count - 1;
            }
        }
        return 0;
    }

    void STDMETHODCALLTYPE EnqueueRequest(
        const GuestDStorageRequest* request) noexcept override {
        RegisteredFixtureRequest approved;
        bool permitted = false;
        {
            // Never dereference a raw guest request pointer. Our own host
            // test explicitly registers a complete, immutable snapshot.
            const std::lock_guard lock(fixture_request_mutex);
            if (const auto it = fixture_requests.find(request);
                it != fixture_requests.end()) {
                approved = std::move(it->second);
                fixture_requests.erase(it); // consume the permit once
                permitted = true;
            }
        }
        HANDLE opened = INVALID_HANDLE_VALUE;
        HRESULT request_error = E_NOTIMPL;
        if (permitted) {
            const auto& r = approved.snapshot;
            const bool approved_vector = approved.destination &&
                r.destination == approved.destination->data() &&
                r.destination_size == approved.destination->size();
            const bool approved_alias = approved.alias &&
                r.destination_size == approved.alias->size();
            if (!r.options && r.file &&
                (approved_vector != approved_alias) &&
                r.destination_size == r.source_size &&
                r.source_size && r.source_size <=
                    HostAsyncFileQueue::kMaxReadBytes &&
                !r.intermediate_size) {
                request_error = reopen_owned_dstorage_file_for_read(
                    r.file, &opened);
            } else {
                request_error = E_INVALIDARG;
            }
        }
        const std::lock_guard lock(mutex_);
        if (closed_) {
            if (opened != INVALID_HANDLE_VALUE) CloseHandle(opened);
            set_error(HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE));
            return;
        }
        retire_markers_locked();
        const uint64_t token = next_token_++;
        if (opened != INVALID_HANDLE_VALUE && SUCCEEDED(request_error)) {
            // The existing worker owns the opened file handle and the
            // approved host-generated buffer until completion. Writeback
            // finishes BEFORE any subsequent status barrier.
            const auto& r = approved.snapshot;
            const HRESULT hr = worker_->enqueue_file(
                opened, token, r.file_offset, r.source_size,
                std::move(approved.destination), std::move(approved.alias));
            if (FAILED(hr)) {
                // Admission failures must never be invisible to a later
                // status barrier. Preserve their FIFO error when capacity
                // permits, otherwise surface the error via the event.
                (void)worker_->enqueue_unsupported(token, hr);
                set_error(hr);
            }
        } else {
            // A rejected request remains an actual FIFO failure; markers
            // never pretend the user's game memory has been filled.
            const HRESULT hr = worker_->enqueue_unsupported(
                token, FAILED(request_error) ? request_error : E_NOTIMPL);
            set_error(FAILED(hr) ? hr :
                (FAILED(request_error) ? request_error : E_NOTIMPL));
        }
    }
    void STDMETHODCALLTYPE EnqueueStatus(
        HostDStorageStatusArrayX* array, uint32_t index) noexcept override {
        if (!array) { set_error(E_POINTER); return; }
        if (!retain_owned_dstorage_status_array(array)) {
            set_error(E_INVALIDARG);
            return;
        }
        // Ownership pin from the registry excludes a concurrent last
        // Release while the worker's AddRef is being installed.
        const std::lock_guard lock(mutex_);
        if (closed_) {
            array->Release();
            set_error(HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE));
            return;
        }
        retire_markers_locked();
        if (marker_count_ >= markers_.size()) {
            array->Release();
            set_error(HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY));
            return;
        }
        const uint64_t token = next_token_++;
        const HRESULT hr = worker_->enqueue_status(token, array, index);
        array->Release();
        if (FAILED(hr)) {
            set_error(hr);
            return;
        }
        markers_[marker_count_++] = token;
        // Queue owns each marker until it completes; retire it promptly
        // on subsequent guest calls so long-lived queues can reuse slots.
    }
    void STDMETHODCALLTYPE EnqueueSignal(
        IUnknown*, uint64_t) noexcept override {
        // No fabricated GPU fence completion. Inject ordered failure.
        const std::lock_guard lock(mutex_);
        if (closed_) {
            set_error(HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE));
            return;
        }
        retire_markers_locked();
        const HRESULT hr = worker_->enqueue_unsupported(next_token_++, E_NOTIMPL);
        set_error(FAILED(hr) ? hr : E_NOTIMPL);
    }
    void STDMETHODCALLTYPE Submit() noexcept override {
        const std::lock_guard lock(mutex_);
        if (closed_) {
            set_error(HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE));
            return;
        }
        worker_->submit_pending();
        retire_markers_locked();
    }
    void STDMETHODCALLTYPE CancelRequestsWithTag(
        uint64_t, uint64_t) noexcept override {
        // Cannot interpret unverified guest request tags. Fail visibly.
        set_error(E_NOTIMPL);
    }
    void STDMETHODCALLTYPE Close() noexcept override {
        const std::lock_guard lock(mutex_);
        if (closed_) return;
        closed_ = true;
        worker_->shutdown();
    }
    HANDLE STDMETHODCALLTYPE GetErrorEvent() noexcept override {
        // Process-owned manual-reset event. Never close this borrowed handle.
        return error_event_;
    }
    void STDMETHODCALLTYPE RetrieveErrorRecord(void*) noexcept override {
        // Unknown Xbox record size: writing guest memory would corrupt it.
        set_error(E_NOTIMPL);
    }
    void STDMETHODCALLTYPE Query(void*) noexcept override {
        // Unknown Xbox output struct size: never write guessed data.
        set_error(E_NOTIMPL);
    }
    void STDMETHODCALLTYPE EnqueueSetEvent(HANDLE) noexcept override {
        // Unsupported guest event lifetime. Never signal arbitrary handles.
        set_error(E_NOTIMPL);
    }
    void STDMETHODCALLTYPE Query1(void*) noexcept override {
        set_error(E_NOTIMPL);
    }

    HRESULT fixture_read(const HostFileReadRequest& request) noexcept {
        const std::lock_guard lock(mutex_);
        if (closed_)
            return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        // Host fixture reads use token space shared with status barriers.
        if (!request.token) return E_INVALIDARG;
        const HRESULT hr = worker_->enqueue(request);
        if (FAILED(hr)) set_error(hr);
        return hr;
    }
    HRESULT fixture_file_read(
        HostDStorageFileX* source, uint64_t token,
        uint64_t offset, uint32_t bytes) noexcept {
        HANDLE opened = INVALID_HANDLE_VALUE;
        const HRESULT hr = reopen_owned_dstorage_file_for_read(
            source, &opened);
        if (FAILED(hr)) return hr;
        const std::lock_guard lock(mutex_);
        if (closed_) {
            CloseHandle(opened);
            return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        }
        // The worker takes unconditional ownership of the independent
        // read handle, even if queue admission is rejected.
        const HRESULT added = worker_->enqueue_file(
            opened, token, offset, bytes);
        if (FAILED(added)) set_error(added);
        return added;
    }
    HRESULT fixture_wait(uint64_t token, DWORD timeout_ms,
                         HostFileReadCompletion& output) noexcept {
        // Do not hold mutex_ while waiting for the worker to finish.
        return worker_->wait_take(token, timeout_ms, output);
    }
    HRESULT last_error() const noexcept {
        return error_.load(std::memory_order_acquire);
    }
private:
    void retire_markers_locked() noexcept {
        size_t retained = 0;
        for (size_t i = 0; i < marker_count_; ++i) {
            HostFileReadCompletion completion;
            if (worker_->try_take(markers_[i], completion) != S_OK)
                markers_[retained++] = markers_[i];
        }
        marker_count_ = retained;
    }
    void set_error(HRESULT error) noexcept {
        if (SUCCEEDED(error)) return;
        HRESULT expected = S_OK;
        (void)error_.compare_exchange_strong(
            expected, error, std::memory_order_acq_rel,
            std::memory_order_relaxed);
        if (error_event_) SetEvent(error_event_);
    }

    std::atomic<ULONG> references_{1};
    std::unique_ptr<HostAsyncFileQueue> worker_;
    HANDLE error_event_ = nullptr;
    std::atomic<HRESULT> error_{S_OK};
    std::mutex mutex_;
    uint64_t next_token_ = (uint64_t{1} << 63); // disjoint from fixture tokens
    std::array<uint64_t, HostAsyncFileQueue::kMaxOutstanding> markers_{};
    size_t marker_count_ = 0;
    bool closed_ = false;
};
} // namespace

HRESULT create_host_dstorage_queue1(
    const void* desc, REFIID iid, void** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    // Enabled ONLY for generated-file tests. Default guest factory path
    // is still explicitly unsupported even with an otherwise valid IID.
    std::filesystem::path root;
    const HRESULT gate = snapshot_dstorage_file_fixture_root(root);
    if (FAILED(gate)) return gate;
    if (!IsEqualGUID(iid, kObservedXboxDStorageQueueIid))
        return E_NOINTERFACE;
    if (!desc) return E_INVALIDARG;
    // Caller must provide a valid descriptor. No GDK header is imported.
    const auto& source = *static_cast<const GuestDStorageQueueDesc1*>(desc);
    // Conservative host fixture limit, not an asserted Xbox API maximum.
    if (source.source_type != 0 || source.capacity != 64 ||
        source.priority < -1 || source.priority > 2 ||
        (source.options & ~uint64_t{1}))
        return E_INVALIDARG;
    try {
        std::unique_ptr<HostAsyncFileQueue> worker;
        const auto policy = (source.options & 1)
            ? HostAsyncFileQueue::SubmitPolicy::Manual
            : HostAsyncFileQueue::SubmitPolicy::HalfCapacity;
        const HRESULT hr = HostAsyncFileQueue::create(root, worker, policy);
        if (FAILED(hr)) return hr;
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) {
            const DWORD last = GetLastError();
            return HRESULT_FROM_WIN32(last ? last : ERROR_GEN_FAILURE);
        }
        auto* queue = new (std::nothrow) HostQueue(
            std::move(worker), event);
        if (!queue) {
            CloseHandle(event);
            return E_OUTOFMEMORY;
        }
        *output = static_cast<HostDStorageQueueX2*>(queue);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HRESULT enqueue_host_dstorage_queue_fixture_read(
    HostDStorageQueueX2* queue,
    const HostFileReadRequest& request) noexcept {
    if (!queue) return E_POINTER;
    if (request.token & (uint64_t{1} << 63))
        return E_INVALIDARG;
    return static_cast<HostQueue*>(queue)->fixture_read(request);
}
HRESULT register_host_dstorage_fixture_request(
    const GuestDStorageRequest* request,
    const std::shared_ptr<std::vector<uint8_t>>& destination) noexcept {
    if (!request || !destination || destination->empty() ||
        destination->size() > HostAsyncFileQueue::kMaxReadBytes)
        return E_INVALIDARG;
    // Called solely by generated host tests, never from a guest export.
    // Copy the full request while it is owned by this trusted producer.
    const GuestDStorageRequest snapshot = *request;
    if (snapshot.destination != destination->data() ||
        snapshot.destination_size != destination->size() ||
        !snapshot.file || snapshot.source_size != snapshot.destination_size ||
        snapshot.options || snapshot.intermediate_size)
        return E_INVALIDARG;
    try {
        const std::lock_guard lock(fixture_request_mutex);
        if (fixture_requests.contains(request))
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        fixture_requests.emplace(request,
            RegisteredFixtureRequest{snapshot, destination, {}});
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}
HRESULT register_host_dstorage_fixture_alias_request(
    const GuestDStorageRequest* request, uintptr_t physical_frame) noexcept {
    if (!request || !physical_frame) return E_INVALIDARG;
    // Trusted fixture helper: only the test-owned structure is read here.
    // Queue EnqueueRequest itself never reads a raw guest request pointer.
    const GuestDStorageRequest snapshot = *request;
    if (snapshot.options || !snapshot.file || !snapshot.destination ||
        !snapshot.destination_size ||
        snapshot.destination_size != snapshot.source_size ||
        snapshot.destination_size > HostDStorageAliasDestination::page_bytes ||
        snapshot.intermediate_size)
        return E_INVALIDARG;
    std::shared_ptr<HostDStorageAliasDestination> pinned;
    const HRESULT hr = HostDStorageAliasDestination::pin(
        snapshot.destination, physical_frame, snapshot.destination_size,
        pinned);
    if (FAILED(hr)) return hr;
    try {
        const std::lock_guard lock(fixture_request_mutex);
        if (fixture_requests.contains(request))
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        fixture_requests.emplace(request,
            RegisteredFixtureRequest{snapshot, {}, std::move(pinned)});
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}
void unregister_host_dstorage_fixture_request(
    const GuestDStorageRequest* request) noexcept {
    const std::lock_guard lock(fixture_request_mutex);
    fixture_requests.erase(request);
}
HRESULT enqueue_host_dstorage_queue_fixture_file_read(
    HostDStorageQueueX2* queue, HostDStorageFileX* source,
    uint64_t token, uint64_t offset, uint32_t bytes) noexcept {
    if (!queue) return E_POINTER;
    if (!token || (token & (uint64_t{1} << 63))) return E_INVALIDARG;
    return static_cast<HostQueue*>(queue)->fixture_file_read(
        source, token, offset, bytes);
}
HRESULT wait_host_dstorage_queue_fixture_read(
    HostDStorageQueueX2* queue, uint64_t token, DWORD timeout_ms,
    HostFileReadCompletion& output) noexcept {
    if (!queue) return E_POINTER;
    if (token & (uint64_t{1} << 63))
        return E_INVALIDARG;
    return static_cast<HostQueue*>(queue)->fixture_wait(
        token, timeout_ms, output);
}
HRESULT last_host_dstorage_queue_error(
    HostDStorageQueueX2* queue) noexcept {
    if (!queue) return E_POINTER;
    return static_cast<HostQueue*>(queue)->last_error();
}
uint32_t live_host_dstorage_queues() noexcept {
    return live_queues.load(std::memory_order_relaxed);
}
} // namespace scarlett::storage
