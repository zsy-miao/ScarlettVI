#include "dstorage_async_queue.h"
#include "dstorage_factory_host.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace scarlett::storage {
namespace {
namespace fs = std::filesystem;
constexpr HRESULT kCancelled = HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
constexpr HRESULT kUnready = HRESULT_FROM_WIN32(ERROR_IO_INCOMPLETE);
constexpr HRESULT kMissing = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
constexpr HRESULT kBusy = HRESULT_FROM_WIN32(ERROR_BUSY);

struct FactoryScope {
    HostDStorageFactoryX2* ptr = nullptr;
    ~FactoryScope() { if (ptr) ptr->Release(); }
};
struct GeneratedFileScope {
    fs::path folder;
    fs::path file;
    bool created = false;
    ~GeneratedFileScope() {
        if (!created) return;
        std::error_code ec;
        fs::remove(file, ec);
        fs::remove(folder, ec);
    }
};
bool read_matches(const HostFileReadCompletion& value,
                  std::span<const uint8_t> source,
                  uint64_t token, uint64_t sequence,
                  uint64_t offset, size_t bytes) noexcept {
    if (value.token != token || value.sequence != sequence ||
        value.status != S_OK || value.data.size() != bytes ||
        offset > source.size() || bytes > source.size() - offset)
        return false;
    return std::equal(value.data.begin(), value.data.end(),
                      source.begin() + offset);
}
} // namespace

bool verify_host_dstorage_submit_gate_fixture() noexcept {
    try {
        GeneratedFileScope generated;
        fs::create_directories(L"reports");
        generated.folder = fs::path(L"reports") /
            (L"dstorage_submit_fixture_" +
             std::to_wstring(GetCurrentProcessId()));
        if (fs::exists(generated.folder) ||
            !fs::create_directory(generated.folder))
            return false;
        generated.created = true;
        generated.file = generated.folder / L"original_test.bin";
        std::vector<uint8_t> source(8192);
        for (size_t i = 0; i < source.size(); ++i)
            source[i] = static_cast<uint8_t>(
                ((i * 131u) + (i >> 3u) * 17u + 31u) & 255u);
        {
            std::ofstream file(generated.file, std::ios::binary);
            if (!file ||
                !file.write(reinterpret_cast<const char*>(source.data()),
                            static_cast<std::streamsize>(source.size())))
                return false;
        }
        FactoryScope factory;
        if (DStorageGetFactory(kObservedXboxDStorageFactoryIid,
                 reinterpret_cast<void**>(&factory.ptr)) != S_OK ||
            !factory.ptr ||
            factory.ptr->SetStagingBufferSize(kDefaultStagingBytes)
                != S_OK)
            return false;

        std::unique_ptr<HostAsyncFileQueue> queue;
        if (HostAsyncFileQueue::create(
                generated.folder, queue,
                HostAsyncFileQueue::SubmitPolicy::Manual) != S_OK ||
            !queue || active_host_dstorage_queue_leases() != 1)
            return false;

        // Held requests must reserve capacity and obtain sequence IDs
        // without touching any file or emitting a completion.
        for (uint64_t token = 11; token <= 13; ++token) {
            if (queue->enqueue({token, L"original_test.bin",
                                token * 19, 128}) != S_OK)
                return false;
        }
        HostFileReadCompletion untouched{};
        untouched.token = 999;
        untouched.status = E_FAIL;
        untouched.data = {0x11,0x22};
        if (queue->pending_submission_count() != 3 ||
            queue->outstanding_count() != 3 ||
            queue->reserved_bytes() != 3 * 128 ||
            queue->try_take(11, untouched) != kUnready ||
            queue->wait_take(13, 0, untouched) !=
                HRESULT_FROM_WIN32(WAIT_TIMEOUT) ||
            untouched.token != 999 || untouched.status != E_FAIL ||
            untouched.data != std::vector<uint8_t>{0x11,0x22})
            return false;

        // Cancelling an unsubmitted request becomes a real error
        // completion; it must never read the source or change order
        // of the two surviving held requests.
        if (queue->cancel_unsubmitted(12) != S_OK ||
            queue->pending_submission_count() != 2 ||
            queue->wait_take(12, 0, untouched) != S_OK ||
            untouched.token != 12 || untouched.sequence != 2 ||
            untouched.status != kCancelled || !untouched.data.empty() ||
            queue->outstanding_count() != 2 ||
            queue->reserved_bytes() != 256 ||
            queue->cancel_unsubmitted(12) != kMissing)
            return false;
        if (queue->submit_pending() != 2 ||
            queue->pending_submission_count() != 0 ||
            queue->submit_pending() != 0)
            return false;
        for (uint64_t token : {11ull, 13ull}) {
            HostFileReadCompletion complete{};
            if (queue->wait_take(token, 10000, complete) != S_OK ||
                !read_matches(complete, source, token, token - 10,
                              token * 19, 128))
                return false;
        }
        if (queue->reserved_bytes() || queue->outstanding_count())
            return false;

        // Immediate submit is a compatibility convenience. It commits
        // earlier held requests *before* its new one in FIFO order.
        for (uint64_t token : {21ull,22ull,23ull}) {
            if (queue->enqueue({token, L"original_test.bin",
                                token * 7, 33}) != S_OK)
                return false;
        }
        if (queue->submit({24, L"original_test.bin", 777, 33})
                    != S_OK ||
            queue->pending_submission_count() != 0 ||
            queue->cancel_unsubmitted(24) != kBusy)
            return false;
        for (uint64_t token = 21; token <= 24; ++token) {
            HostFileReadCompletion complete{};
            const uint64_t sequence = token - 17; // 4..7
            const uint64_t offset = token == 24 ? 777 : token * 7;
            if (queue->wait_take(token, 10000, complete) != S_OK ||
                !read_matches(complete, source, token,
                              sequence, offset, 33))
                return false;
        }
        if (queue->outstanding_count() || queue->reserved_bytes())
            return false;

        // An unsubmitted request can be shut down without ever doing IO.
        if (queue->enqueue({30, L"original_test.bin", 0, 256})
                    != S_OK ||
            queue->enqueue({31, L"original_test.bin", 64, 256})
                    != S_OK ||
            queue->pending_submission_count() != 2)
            return false;
        queue->shutdown();
        if (queue->pending_submission_count() != 0 ||
            queue->submit_pending() != 0 ||
            queue->enqueue({32, L"original_test.bin", 0, 1}) !=
                HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE))
            return false;
        for (uint64_t token : {30ull,31ull}) {
            HostFileReadCompletion complete{};
            if (queue->wait_take(token, 0, complete) != S_OK ||
                complete.status != kCancelled || complete.token != token ||
                !complete.data.empty())
                return false;
        }
        queue.reset();
        if (active_host_dstorage_queue_leases() != 0)
            return false;

        // A new host queue mirrors the documented half-capacity
        // automatic submission threshold (32/64). The first 31 held
        // requests do not start; request 32 commits all 32.
        if (HostAsyncFileQueue::create(
                generated.folder, queue,
                HostAsyncFileQueue::SubmitPolicy::HalfCapacity) != S_OK)
            return false;
        for (uint64_t i = 1; i <= 31; ++i)
            if (queue->enqueue({i, L"original_test.bin",
                                i * 13, 47}) != S_OK)
                return false;
        if (queue->pending_submission_count() != 31 ||
            queue->try_take(1, untouched) != kUnready ||
            queue->reserved_bytes() != 31 * 47)
            return false;
        if (queue->enqueue(
                {32, L"original_test.bin", 32*13, 47}) != S_OK ||
            queue->pending_submission_count() != 0 ||
            queue->outstanding_count() != 32)
            return false;
        for (uint64_t i = 1; i <= 32; ++i) {
            HostFileReadCompletion result{};
            if (queue->wait_take(i, 10000, result) != S_OK ||
                !read_matches(result, source, i, i, i*13, 47))
                return false;
        }

        // Multiple producers racing to reach the next auto threshold
        // cannot double-commit work or lose sequence IDs.
        std::atomic<bool> passed{true};
        std::vector<std::jthread> producers;
        for (uint64_t thread = 0; thread < 4; ++thread) {
            producers.emplace_back([&, thread] {
                for (uint64_t n = 0; n < 8; ++n) {
                    const uint64_t token = 100 + thread*8 + n;
                    if (queue->enqueue({
                            token, L"original_test.bin",
                            (token * 29) % (source.size()-51), 51
                        }) != S_OK)
                        passed.store(false);
                }
            });
        }
        for (auto& producer : producers) producer.join();
        if (!passed.load() || queue->pending_submission_count() != 0 ||
            queue->outstanding_count() != 32)
            return false;
        std::array<bool,32> sequence_seen{};
        for (uint64_t token = 100; token < 132; ++token) {
            HostFileReadCompletion result{};
            if (queue->wait_take(token, 10000, result) != S_OK ||
                result.status != S_OK ||
                result.sequence < 33 || result.sequence > 64 ||
                sequence_seen[result.sequence - 33] ||
                !read_matches(result, source, token, result.sequence,
                              (token * 29) % (source.size()-51), 51))
                return false;
            sequence_seen[result.sequence - 33] = true;
        }
        if (!std::all_of(sequence_seen.begin(), sequence_seen.end(),
                         [](bool flag){return flag;}) ||
            queue->outstanding_count() || queue->reserved_bytes())
            return false;

        // A final batch is below the auto threshold; explicitly flush.
        for (uint64_t token = 200; token < 205; ++token)
            if (queue->enqueue({
                    token, L"original_test.bin", token*17, 24
                }) != S_OK)
                return false;
        if (queue->pending_submission_count() != 5 ||
            queue->submit_pending() != 5)
            return false;
        for (uint64_t token = 200; token < 205; ++token) {
            HostFileReadCompletion result{};
            if (queue->wait_take(token, 10000, result) != S_OK ||
                !read_matches(result, source, token, token-135,
                              token*17, 24))
                return false;
        }
        queue.reset();
        return active_host_dstorage_queue_leases() == 0 &&
               factory.ptr->SetStagingBufferSize(
                   kDefaultStagingBytes) == S_OK;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::storage
