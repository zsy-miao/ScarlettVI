#include "dstorage_async_queue.h"
#include "dstorage_alias_destination.h"
#include "../runtime/xmem_bridge.h"
#include "dstorage_factory_host.h"
#include "dstorage_status_host.h"
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <iterator>
#include <limits>
#include <utility>

namespace scarlett::storage {
namespace {
constexpr HRESULT kIoPending = HRESULT_FROM_WIN32(ERROR_IO_INCOMPLETE);
constexpr HRESULT kIoMissing = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
constexpr HRESULT kIoCancelled = HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
constexpr HRESULT kIoExhausted = HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
constexpr HRESULT kIoClosed = HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);

struct OwnedHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~OwnedHandle() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};

HRESULT error_from_last() noexcept {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
}

HostAsyncFileQueue::HostAsyncFileQueue(
    std::filesystem::path trusted_root, SubmitPolicy policy)
    : root_(std::move(trusted_root)), policy_(policy) {
    worker_ = std::thread([this] { run(); });
}

HRESULT HostAsyncFileQueue::create(
    const std::filesystem::path& trusted_root,
    std::unique_ptr<HostAsyncFileQueue>& output,
    SubmitPolicy policy) noexcept {
    output.reset();
    try {
        const auto absolute = std::filesystem::absolute(trusted_root);
        const DWORD attributes = GetFileAttributesW(absolute.c_str());
        // No root junction, symlink, missing path, or non-directory root.
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
        if (!acquire_host_dstorage_queue_lease())
            return HRESULT_FROM_WIN32(ERROR_NOT_READY);
        try {
            std::unique_ptr<HostAsyncFileQueue> queue(
                new HostAsyncFileQueue(absolute, policy));
            queue->leased_ = true;
            output = std::move(queue);
            return S_OK;
        } catch (...) {
            release_host_dstorage_queue_lease();
            throw;
        }
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HostAsyncFileQueue::~HostAsyncFileQueue() noexcept {
    shutdown();
    // Worker has joined; release marker-held COM references for any
    // unconsumed completions before the host staging lease is returned.
    for (auto& [token, job] : jobs_)
        if (job.status) job.status->Release();
    jobs_.clear();
    if (leased_) {
        release_host_dstorage_queue_lease();
        leased_ = false;
    }
}

bool HostAsyncFileQueue::allowed_leaf(
    const std::wstring& leaf) noexcept {
    if (leaf.empty() || leaf.size() > 96 ||
        leaf == L"." || leaf == L".." ||
        leaf.back() == L'.')
        return false;
    for (const wchar_t c : leaf) {
        const bool ascii_letter = (c >= L'a' && c <= L'z') ||
                                  (c >= L'A' && c <= L'Z');
        const bool ascii_digit = c >= L'0' && c <= L'9';
        if (!ascii_letter && !ascii_digit &&
            c != L'-' && c != L'_' && c != L'.')
            return false;
    }
    // Windows treats these names as devices even with an extension.
    const auto dot = leaf.find(L'.');
    std::wstring stem = leaf.substr(0, dot);
    for (auto& c : stem) c = std::towupper(c);
    if (stem == L"CON" || stem == L"PRN" ||
        stem == L"AUX" || stem == L"NUL")
        return false;
    if (stem.size() == 4 &&
        (stem.substr(0,3) == L"COM" ||
         stem.substr(0,3) == L"LPT") &&
        stem[3] >= L'1' && stem[3] <= L'9')
        return false;
    return true;
}

HRESULT HostAsyncFileQueue::admit(
    const HostFileReadRequest& request, bool staged,
    HostDStorageStatusArrayX* status, uint32_t status_index,
    HRESULT injected_failure, HANDLE owned_handle,
    std::shared_ptr<std::vector<uint8_t>> destination,
    std::shared_ptr<HostDStorageAliasDestination> alias,
    std::shared_ptr<xmem::HostXMemWritePin> heap) noexcept {
    OwnedFileHandle incoming(owned_handle); // consumed even on validation failure
    const unsigned destinations = static_cast<unsigned>(bool(destination)) +
                                  static_cast<unsigned>(bool(alias)) +
                                  static_cast<unsigned>(bool(heap));
    if (!request.token || destinations > 1 ||
        (destinations && owned_handle == INVALID_HANDLE_VALUE) ||
        (destination && destination->size() != request.bytes) ||
        (alias && alias->size() != request.bytes) ||
        (heap && heap->size() != request.bytes)) return E_INVALIDARG;
    if (FAILED(injected_failure)) {
        if (!staged || status || owned_handle != INVALID_HANDLE_VALUE ||
            !request.filename.empty() || request.offset || request.bytes)
            return E_INVALIDARG;
    } else if (status) {
        if (!staged || owned_handle != INVALID_HANDLE_VALUE ||
            !request.filename.empty() || request.offset || request.bytes)
            return E_INVALIDARG;
    } else if ((owned_handle == INVALID_HANDLE_VALUE &&
                !allowed_leaf(request.filename)) ||
               (owned_handle != INVALID_HANDLE_VALUE &&
                !request.filename.empty()) ||
               request.offset > static_cast<uint64_t>(
                   std::numeric_limits<int64_t>::max()) ||
               !request.bytes || request.bytes > kMaxReadBytes)
        return E_INVALIDARG;
    try {
        const std::lock_guard lock(mutex_);
        if (stopped_) return kIoClosed;
        if (jobs_.contains(request.token))
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        if (jobs_.size() >= kMaxOutstanding ||
            request.bytes > kMaxReservedBytes - reserved_bytes_)
            return kIoExhausted;

        Job job;
        job.request = request; // copy guest-independent host leaf
        job.file_handle = std::move(incoming);
        job.destination = std::move(destination);
        job.alias = std::move(alias);
        job.heap = std::move(heap);
        job.status = status;
        job.status_index = status_index;
        job.injected_failure = injected_failure;
        job.result.token = request.token;
        job.result.sequence = next_sequence_;
        const auto [it, inserted] =
            jobs_.try_emplace(request.token, std::move(job));
        if (!inserted)
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        try {
            if (staged) {
                pending_.push_back(request.token);
            } else {
                fifo_.push_back(request.token);
            }
        } catch (...) {
            jobs_.erase(it);
            throw;
        }
        if (status) {
            // Begin while queue admission is locked, before the worker
            // can see a committed marker. Failure rolls back insertion
            // and doesn't change sequence or resource quotas.
            const HRESULT status_hr = begin_host_dstorage_status(
                status, status_index);
            if (FAILED(status_hr)) {
                if (staged) pending_.pop_back();
                else fifo_.pop_back();
                jobs_.erase(it);
                return status_hr;
            }
            status->AddRef();
        }
        ++next_sequence_;
        reserved_bytes_ += request.bytes;
        if (!staged) {
            // Inserting the new request first keeps failure atomic:
            // then splice all older staged requests BEFORE it.
            const auto last = std::prev(fifo_.end());
            fifo_.splice(last, pending_);
            ready_.notify_one();
        } else if (policy_ == SubmitPolicy::HalfCapacity &&
                   pending_.size() >= kMaxOutstanding / 2) {
            // Real auto-commit at half capacity; no heap allocation.
            fifo_.splice(fifo_.end(), pending_);
            ready_.notify_one();
        }
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HRESULT HostAsyncFileQueue::enqueue(
    const HostFileReadRequest& request) noexcept {
    return admit(request, true);
}
HRESULT HostAsyncFileQueue::enqueue_file(
    HANDLE owned_handle, uint64_t token,
    uint64_t offset, uint32_t bytes,
    std::shared_ptr<std::vector<uint8_t>> destination,
    std::shared_ptr<HostDStorageAliasDestination> alias,
    std::shared_ptr<xmem::HostXMemWritePin> heap) noexcept {
    if (owned_handle == nullptr || owned_handle == INVALID_HANDLE_VALUE) {
        if (owned_handle && owned_handle != INVALID_HANDLE_VALUE)
            CloseHandle(owned_handle);
        return E_INVALIDARG;
    }
    return admit({token, L"", offset, bytes}, true, nullptr, 0,
                 S_OK, owned_handle, std::move(destination),
                 std::move(alias), std::move(heap));
}
HRESULT HostAsyncFileQueue::enqueue_unsupported(
    uint64_t token, HRESULT error) noexcept {
    if (!FAILED(error)) return E_INVALIDARG;
    return admit({token, L"", 0, 0}, true, nullptr, 0, error);
}
HRESULT HostAsyncFileQueue::enqueue_status(
    uint64_t token, HostDStorageStatusArrayX* status,
    uint32_t index) noexcept {
    if (!status) return E_POINTER;
    return admit({token, L"", 0, 0}, true, status, index);
}
HRESULT HostAsyncFileQueue::submit(
    const HostFileReadRequest& request) noexcept {
    return admit(request, false);
}
size_t HostAsyncFileQueue::submit_pending() noexcept {
    const std::lock_guard lock(mutex_);
    if (stopped_) return 0;
    const size_t count = pending_.size();
    if (count) {
        fifo_.splice(fifo_.end(), pending_);
        ready_.notify_one();
    }
    return count;
}
HRESULT HostAsyncFileQueue::cancel_unsubmitted(
    uint64_t token) noexcept {
    const std::lock_guard lock(mutex_);
    const auto job = jobs_.find(token);
    if (job == jobs_.end())
        return kIoMissing;
    const auto pending = std::find(
        pending_.begin(), pending_.end(), token);
    if (pending == pending_.end())
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    pending_.erase(pending);
    job->second.file_handle.reset();
    job->second.destination.reset();
    job->second.alias.reset();
    job->second.heap.reset();
    job->second.result.status = kIoCancelled;
    job->second.result.data.clear();
    if (job->second.status) {
        (void)finish_host_dstorage_status(job->second.status,
                                          job->second.status_index,
                                          kIoCancelled);
    }
    job->second.ready = true;
    ready_.notify_all();
    return S_OK;
}
size_t HostAsyncFileQueue::pending_submission_count() const noexcept {
    const std::lock_guard lock(mutex_);
    return pending_.size();
}

HostFileReadCompletion HostAsyncFileQueue::execute(
    const HostFileReadRequest& request,
    uint64_t sequence, HANDLE owned_handle) noexcept {
    HostFileReadCompletion completion;
    completion.token = request.token;
    completion.sequence = sequence;
    OwnedHandle file{owned_handle};
    try {
        if (file.handle == INVALID_HANDLE_VALUE) {
            const std::filesystem::path path = root_ / request.filename;
            file.handle = CreateFileW(
                path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
        }
        if (file.handle == INVALID_HANDLE_VALUE) {
            completion.status = error_from_last();
            return completion;
        }
        FILE_ATTRIBUTE_TAG_INFO file_info{};
        if (!GetFileInformationByHandleEx(
                file.handle, FileAttributeTagInfo,
                &file_info, sizeof(file_info))) {
            completion.status = error_from_last();
            return completion;
        }
        if (file_info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                                        FILE_ATTRIBUTE_DIRECTORY)) {
            completion.status =
                HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
            return completion;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.handle, &size)) {
            completion.status = error_from_last();
            return completion;
        }
        if (size.QuadPart < 0 ||
            request.offset > static_cast<uint64_t>(size.QuadPart) ||
            request.bytes > static_cast<uint64_t>(size.QuadPart) -
                                request.offset) {
            completion.status =
                HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
            return completion;
        }
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(request.offset);
        if (!SetFilePointerEx(file.handle, position, nullptr, FILE_BEGIN)) {
            completion.status = error_from_last();
            return completion;
        }
        completion.data.resize(request.bytes);
        uint32_t copied = 0;
        while (copied < request.bytes) {
            DWORD read = 0;
            if (!ReadFile(file.handle, completion.data.data() + copied,
                          request.bytes - copied, &read, nullptr)) {
                completion.status = error_from_last();
                completion.data.clear();
                return completion;
            }
            if (!read) {
                completion.status =
                    HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
                completion.data.clear();
                return completion;
            }
            copied += read;
        }
        completion.status = S_OK;
        return completion;
    } catch (...) {
        completion.data.clear();
        completion.status = E_OUTOFMEMORY;
        return completion;
    }
}

void HostAsyncFileQueue::run() noexcept {
    while (true) {
        uint64_t token = 0;
        HostFileReadRequest request;
        uint64_t sequence = 0;
        HostDStorageStatusArrayX* status = nullptr;
        uint32_t status_index = 0;
        HRESULT injected_failure = S_OK;
        HANDLE read_handle = INVALID_HANDLE_VALUE;
        std::shared_ptr<std::vector<uint8_t>> destination;
        std::shared_ptr<HostDStorageAliasDestination> alias;
        std::shared_ptr<xmem::HostXMemWritePin> heap;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [&]{ return stopped_ || !fifo_.empty(); });
            if (fifo_.empty()) {
                if (stopped_) return;
                continue;
            }
            token = fifo_.front();
            fifo_.pop_front();
            const auto found = jobs_.find(token);
            if (found == jobs_.end()) continue;
            // Move the already-owned name without allocating inside the
            // noexcept worker loop. Keep scalar byte count in the map so
            // completion retirement can release the admission charge.
            request = std::move(found->second.request);
            sequence = found->second.result.sequence;
            status = found->second.status;
            status_index = found->second.status_index;
            injected_failure = found->second.injected_failure;
            read_handle = found->second.file_handle.release();
            destination = std::move(found->second.destination);
            alias = std::move(found->second.alias);
            heap = std::move(found->second.heap);
        }
        HostFileReadCompletion completion;
        if (status) {
            // A genuine worker barrier: aggregate the FIRST error from
            // preceding committed reads since the prior status marker.
            completion.token = token;
            completion.sequence = sequence;
            completion.status = first_unreported_failure_;
            (void)finish_host_dstorage_status(
                status, status_index, completion.status);
            first_unreported_failure_ = S_OK;
        } else {
            if (FAILED(injected_failure)) {
                completion.token = token;
                completion.sequence = sequence;
                completion.status = injected_failure;
            } else {
                completion = execute(request, sequence, read_handle);
                if (SUCCEEDED(completion.status) && alias) {
                    completion.status = alias->write(
                        completion.data.data(), completion.data.size());
                    if (FAILED(completion.status)) completion.data.clear();
                }
                if (SUCCEEDED(completion.status) && heap) {
                    completion.status = heap->write(
                        completion.data.data(), completion.data.size());
                    if (FAILED(completion.status)) completion.data.clear();
                }
                if (SUCCEEDED(completion.status) && destination) {
                    // Write the accepted, immutable-size host fixture
                    // destination BEFORE a subsequent FIFO status marker.
                    if (destination->size() != completion.data.size()) {
                        completion.status = E_INVALIDARG;
                        completion.data.clear();
                    } else {
                        std::copy(completion.data.begin(),
                                  completion.data.end(),
                                  destination->begin());
                    }
                }
            }
            if (FAILED(completion.status) &&
                SUCCEEDED(first_unreported_failure_))
                first_unreported_failure_ = completion.status;
        }
        {
            const std::lock_guard lock(mutex_);
            if (const auto found = jobs_.find(token);
                found != jobs_.end() && !found->second.ready) {
                if (FAILED(injected_failure)) {
                    // This token was injected only to preserve an ordered
                    // error barrier. No guest-facing consumer exists; do
                    // not let it exhaust the bounded queue after Submit.
                    jobs_.erase(found);
                } else {
                    found->second.result = std::move(completion);
                    found->second.ready = true;
                }
            }
        }
        ready_.notify_all();
    }
}

HRESULT HostAsyncFileQueue::take_locked(
    std::map<uint64_t, Job>::iterator it,
    HostFileReadCompletion& output) noexcept {
    if (it == jobs_.end()) return kIoMissing;
    if (!it->second.ready) return kIoPending;
    output = std::move(it->second.result);
    reserved_bytes_ -= it->second.request.bytes;
    if (it->second.status) it->second.status->Release();
    jobs_.erase(it);
    return S_OK;
}
HRESULT HostAsyncFileQueue::try_take(
    uint64_t token, HostFileReadCompletion& output) noexcept {
    const std::lock_guard lock(mutex_);
    return take_locked(jobs_.find(token), output);
}
HRESULT HostAsyncFileQueue::wait_take(
    uint64_t token, DWORD timeout_ms,
    HostFileReadCompletion& output) noexcept {
    std::unique_lock lock(mutex_);
    if (!jobs_.contains(token)) return kIoMissing;
    if (!ready_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [&] {
        const auto found = jobs_.find(token);
        return found == jobs_.end() || found->second.ready;
    }))
        return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    return take_locked(jobs_.find(token), output);
}
void HostAsyncFileQueue::shutdown() noexcept {
    {
        const std::lock_guard lock(mutex_);
        if (!stopped_) {
            stopped_ = true;
            const auto cancel_held = [&](const auto& list) {
                for (const uint64_t id : list) {
                    if (auto it = jobs_.find(id); it != jobs_.end()) {
                        it->second.file_handle.reset();
                        it->second.destination.reset();
                        it->second.alias.reset();
                        it->second.heap.reset();
                        it->second.result.status = kIoCancelled;
                        it->second.result.data.clear();
                        it->second.ready = true;
                        if (it->second.status) {
                            (void)finish_host_dstorage_status(
                                it->second.status,
                                it->second.status_index,
                                kIoCancelled);
                        }
                    }
                }
            };
            cancel_held(pending_);
            cancel_held(fifo_);
            pending_.clear();
            fifo_.clear();
            /* The already-running read is joined below. */
        }
    }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}
size_t HostAsyncFileQueue::outstanding_count() const noexcept {
    const std::lock_guard lock(mutex_);
    return jobs_.size();
}
uint64_t HostAsyncFileQueue::reserved_bytes() const noexcept {
    const std::lock_guard lock(mutex_);
    return reserved_bytes_;
}
} // namespace scarlett::storage
