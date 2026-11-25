#include "dstorage_async_queue.h"
#include "dstorage_factory_host.h"
#include <algorithm>
#include <chrono>
#include <cwctype>
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
    std::filesystem::path trusted_root)
    : root_(std::move(trusted_root)) {
    worker_ = std::thread([this] { run(); });
}

HRESULT HostAsyncFileQueue::create(
    const std::filesystem::path& trusted_root,
    std::unique_ptr<HostAsyncFileQueue>& output) noexcept {
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
                new HostAsyncFileQueue(absolute));
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

HRESULT HostAsyncFileQueue::submit(
    const HostFileReadRequest& request) noexcept {
    if (!request.token || !allowed_leaf(request.filename) ||
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
        job.result.token = request.token;
        job.result.sequence = next_sequence_;
        const auto [it, inserted] =
            jobs_.try_emplace(request.token, std::move(job));
        if (!inserted)
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        try {
            fifo_.push_back(request.token);
        } catch (...) {
            jobs_.erase(it);
            throw;
        }
        ++next_sequence_;
        reserved_bytes_ += request.bytes;
        ready_.notify_one();
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HostFileReadCompletion HostAsyncFileQueue::execute(
    const HostFileReadRequest& request,
    uint64_t sequence) noexcept {
    HostFileReadCompletion completion;
    completion.token = request.token;
    completion.sequence = sequence;
    try {
        const std::filesystem::path path = root_ / request.filename;
        OwnedHandle file{
            CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                        nullptr)};
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
        }
        auto completion = execute(request, sequence);
        {
            const std::lock_guard lock(mutex_);
            if (const auto found = jobs_.find(token);
                found != jobs_.end() && !found->second.ready) {
                found->second.result = std::move(completion);
                found->second.ready = true;
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
            for (const uint64_t id : fifo_) {
                if (auto it = jobs_.find(id); it != jobs_.end()) {
                    it->second.result.status = kIoCancelled;
                    it->second.result.data.clear();
                    it->second.ready = true;
                }
            }
            fifo_.clear();
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
