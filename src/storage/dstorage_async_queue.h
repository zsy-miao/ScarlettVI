#pragma once
#include <Windows.h>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace scarlett::storage {

// Entirely HOST-ONLY request format. This is NOT a DSTORAGE_REQUEST,
// not a guest COM IDStorageQueueX, and never exposed to the title.
struct HostFileReadRequest {
    uint64_t token = 0;
    std::wstring filename; // exactly one validated leaf, never a guest path
    uint64_t offset = 0;
    uint32_t bytes = 0;
};
struct HostFileReadCompletion {
    uint64_t token = 0;
    uint64_t sequence = 0; // monotonically increasing submission FIFO index
    HRESULT status = E_PENDING;
    std::vector<uint8_t> data; // populated only on exact successful read
};

class HostAsyncFileQueue final {
public:
    static constexpr uint32_t kMaxReadBytes = 1024 * 1024;
    static constexpr size_t kMaxOutstanding = 64;
    static constexpr uint64_t kMaxReservedBytes = 8ull * 1024 * 1024;

    // Host supplies a trusted directory. It must be a non-reparse
    // directory; every request contains a SINGLE validated leaf, and
    // opened file handles refuse reparse points / directory objects.
    static HRESULT create(
        const std::filesystem::path& trusted_root,
        std::unique_ptr<HostAsyncFileQueue>& output) noexcept;

    ~HostAsyncFileQueue() noexcept;
    HostAsyncFileQueue(const HostAsyncFileQueue&) = delete;
    HostAsyncFileQueue& operator=(const HostAsyncFileQueue&) = delete;

    HRESULT submit(const HostFileReadRequest& request) noexcept;
    // Returns the *operation's* status in completion.status. A successful
    // wait/take returns S_OK even when a file read failed.
    HRESULT try_take(uint64_t token, HostFileReadCompletion& output) noexcept;
    HRESULT wait_take(uint64_t token, DWORD timeout_ms,
                      HostFileReadCompletion& output) noexcept;
    void shutdown() noexcept;
    size_t outstanding_count() const noexcept;
    uint64_t reserved_bytes() const noexcept;

private:
    explicit HostAsyncFileQueue(std::filesystem::path trusted_root);
    struct Job {
        HostFileReadRequest request;
        HostFileReadCompletion result;
        bool ready = false;
    };
    void run() noexcept;
    HostFileReadCompletion execute(
        const HostFileReadRequest& request, uint64_t sequence) noexcept;
    HRESULT take_locked(
        std::map<uint64_t, Job>::iterator it,
        HostFileReadCompletion& output) noexcept;
    static bool allowed_leaf(const std::wstring& leaf) noexcept;

    const std::filesystem::path root_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<uint64_t> fifo_;
    std::map<uint64_t, Job> jobs_;
    std::thread worker_;
    uint64_t reserved_bytes_ = 0;
    uint64_t next_sequence_ = 1;
    bool stopped_ = false;
    bool leased_ = false;
};

bool verify_host_dstorage_async_queue_fixture() noexcept;

} // namespace scarlett::storage
