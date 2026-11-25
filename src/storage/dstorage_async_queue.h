#pragma once
#include <Windows.h>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <list>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace scarlett::storage {
struct HostDStorageStatusArrayX;

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

    // Entirely HOST-ONLY submission policy; these are not Xbox queue flags.
    // HalfCapacity mirrors the public Xbox auto-submit trigger at 32
    // requests, while Manual requires an explicit submit_pending call.
    enum class SubmitPolicy { Manual, HalfCapacity };

    // Host supplies a trusted directory. It must be a non-reparse
    // directory; every request contains a SINGLE validated leaf, and
    // opened file handles refuse reparse points / directory objects.
    static HRESULT create(
        const std::filesystem::path& trusted_root,
        std::unique_ptr<HostAsyncFileQueue>& output,
        SubmitPolicy policy = SubmitPolicy::Manual) noexcept;

    ~HostAsyncFileQueue() noexcept;
    HostAsyncFileQueue(const HostAsyncFileQueue&) = delete;
    HostAsyncFileQueue& operator=(const HostAsyncFileQueue&) = delete;

    // Hold work without touching a file until submit_pending() commits it.
    HRESULT enqueue(const HostFileReadRequest& request) noexcept;
    // Host-only FIFO status barrier. It is NOT IDStorageQueueX::EnqueueStatus;
    // the worker finishes it with the first real preceding read error.
    HRESULT enqueue_status(uint64_t token,
                           HostDStorageStatusArrayX* status,
                           uint32_t index) noexcept;
    // Commit all held work in FIFO order; no allocation or blocking I/O.
    // Returns the number of newly committed host requests.
    size_t submit_pending() noexcept;
    // Compatibility convenience: admit and commit this request and any
    // older staged requests atomically in submission order.
    HRESULT submit(const HostFileReadRequest& request) noexcept;
    // Cancel a host-only request that has not yet been submitted;
    // cancellation becomes a genuine completion that must be retired.
    HRESULT cancel_unsubmitted(uint64_t token) noexcept;
    size_t pending_submission_count() const noexcept;
    // Returns the *operation's* status in completion.status. A successful
    // wait/take returns S_OK even when a file read failed.
    HRESULT try_take(uint64_t token, HostFileReadCompletion& output) noexcept;
    HRESULT wait_take(uint64_t token, DWORD timeout_ms,
                      HostFileReadCompletion& output) noexcept;
    void shutdown() noexcept;
    size_t outstanding_count() const noexcept;
    uint64_t reserved_bytes() const noexcept;

private:
    explicit HostAsyncFileQueue(std::filesystem::path trusted_root,
                                SubmitPolicy policy);
    struct Job {
        HostFileReadRequest request;
        HostDStorageStatusArrayX* status = nullptr;
        uint32_t status_index = 0;
        HostFileReadCompletion result;
        bool ready = false;
    };
    HRESULT admit(const HostFileReadRequest& request,
                  bool staged, HostDStorageStatusArrayX* status = nullptr,
                  uint32_t status_index = 0) noexcept;
    void run() noexcept;
    HostFileReadCompletion execute(
        const HostFileReadRequest& request, uint64_t sequence) noexcept;
    HRESULT take_locked(
        std::map<uint64_t, Job>::iterator it,
        HostFileReadCompletion& output) noexcept;
    static bool allowed_leaf(const std::wstring& leaf) noexcept;

    const std::filesystem::path root_;
    const SubmitPolicy policy_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    // std::list::splice transfers staged nodes without allocation or
    // iterator invalidation, guaranteeing no partial commit on OOM.
    std::list<uint64_t> pending_;
    std::list<uint64_t> fifo_;
    std::map<uint64_t, Job> jobs_;
    std::thread worker_;
    uint64_t reserved_bytes_ = 0;
    uint64_t next_sequence_ = 1;
    HRESULT first_unreported_failure_ = S_OK;
    bool stopped_ = false;
    bool leased_ = false;
};

bool verify_host_dstorage_async_queue_fixture() noexcept;
bool verify_host_dstorage_submit_gate_fixture() noexcept;

} // namespace scarlett::storage
