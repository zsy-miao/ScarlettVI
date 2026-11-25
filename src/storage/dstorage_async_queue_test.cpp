#include "dstorage_async_queue.h"
#include "dstorage_factory_host.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>

namespace scarlett::storage {
namespace {
namespace fs = std::filesystem;
constexpr uint32_t kMiB = 1024 * 1024;
constexpr uint32_t kLargeRead = 128 * 1024;
struct OwnedFactory {
    HostDStorageFactoryX2* object = nullptr;
    ~OwnedFactory() { if (object) object->Release(); }
};
struct FixtureFiles {
    fs::path root;
    fs::path file;
    bool created = false;
    ~FixtureFiles() {
        if (!created) return;
        std::error_code ec;
        fs::remove(file, ec);
        fs::remove(root, ec);
    }
};
bool matches(const std::vector<uint8_t>& actual,
             const std::vector<uint8_t>& source, size_t offset) {
    if (offset > source.size() ||
        actual.size() > source.size() - offset) return false;
    return std::equal(actual.begin(), actual.end(),
                      source.begin() + offset);
}
} // namespace

bool verify_host_dstorage_async_queue_fixture() noexcept {
    try {
        FixtureFiles cleanup;
        const fs::path reports = L"reports";
        fs::create_directories(reports);
        cleanup.root = reports /
            (L"dstorage_async_fixture_" +
             std::to_wstring(GetCurrentProcessId()));
        if (fs::exists(cleanup.root))
            return false; // no removal of unrelated/preexisting data
        if (!fs::create_directory(cleanup.root))
            return false;
        cleanup.created = true;
        cleanup.file = cleanup.root / L"generated.bin";

        std::vector<uint8_t> source(2*kMiB + 4096 + 23);
        for (size_t i = 0; i < source.size(); ++i)
            source[i] = static_cast<uint8_t>(
                ((i * 73u) ^ (i >> 9u) ^ (i >> 17u) ^ 0x5du) & 255u);
        {
            std::ofstream file(cleanup.file, std::ios::binary);
            if (!file ||
                !file.write(reinterpret_cast<const char*>(source.data()),
                            static_cast<std::streamsize>(source.size())))
                return false;
        }

        OwnedFactory factory{};
        if (DStorageGetFactory(
                kObservedXboxDStorageFactoryIid,
                reinterpret_cast<void**>(&factory.object)) != S_OK ||
            !factory.object || !host_dstorage_staging_bytes())
            return false;
        if (factory.object->SetStagingBufferSize(32*kMiB) != S_OK ||
            active_host_dstorage_queue_leases() != 0)
            return false;

        std::unique_ptr<HostAsyncFileQueue> queue;
        if (HostAsyncFileQueue::create(
                cleanup.root / L"missing-directory", queue) == S_OK ||
            queue || active_host_dstorage_queue_leases() != 0)
            return false;
        if (HostAsyncFileQueue::create(cleanup.root, queue) != S_OK ||
            !queue || active_host_dstorage_queue_leases() != 1)
            return false;

        const auto busy = HRESULT_FROM_WIN32(ERROR_BUSY);
        if (factory.object->SetStagingBufferSize(32*kMiB) != busy ||
            factory.object->SetStagingBufferSize(16*kMiB) != busy ||
            host_dstorage_staging_bytes() != 32*kMiB)
            return false;

        HostFileReadCompletion untouched;
        untouched.token = 444;
        untouched.status = E_FAIL;
        untouched.data = {0xCC};
        if (queue->try_take(500, untouched) !=
                HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
            queue->wait_take(500, 0, untouched) !=
                HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
            untouched.token != 444 || untouched.data !=
                std::vector<uint8_t>{0xCC})
            return false;

        // Path containment: no parent traversal, ADS, subfolder,
        // reserved Windows device names, symlink path or empty name.
        for (const auto& path : {
                 L"", L"..", L"../game.bin", L"nested\\game.bin",
                 L"game:stream", L"CON", L"NUL.txt",
                 L"LPT1.log", L"generated.bin.", L"C:\\game.bin"}) {
            if (queue->submit({600, path, 0, 1}) != E_INVALIDARG)
                return false;
        }
        if (queue->submit({0, L"generated.bin", 0, 1})
                    != E_INVALIDARG ||
            queue->submit({601, L"generated.bin", 0, 0})
                    != E_INVALIDARG ||
            queue->submit({602, L"generated.bin", 0,
                           HostAsyncFileQueue::kMaxReadBytes + 1})
                    != E_INVALIDARG ||
            queue->submit({603, L"generated.bin",
                           std::numeric_limits<uint64_t>::max(), 1})
                    != E_INVALIDARG ||
            queue->outstanding_count() != 0 ||
            queue->reserved_bytes() != 0)
            return false;

        // Exactly 64 * 128KiB = 8MiB admission: all accepted and
        // reserved regardless of how rapidly the worker finishes.
        for (uint64_t i = 1; i <= 64; ++i) {
            const uint64_t offset =
                (i * 3701u) % (source.size() - kLargeRead);
            if (queue->submit(
                    {i, L"generated.bin", offset, kLargeRead}) != S_OK)
                return false;
        }
        if (queue->outstanding_count() != 64 ||
            queue->reserved_bytes() != 8ull*kMiB ||
            queue->submit({65, L"generated.bin", 0, 1}) !=
                HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY) ||
            queue->submit({1, L"generated.bin", 0, 1}) !=
                HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
            return false;

        // The worker consumes submitted entries in exact FIFO order.
        // Completions are independently owned, with full verified bytes.
        for (uint64_t i = 1; i <= 64; ++i) {
            HostFileReadCompletion complete;
            if (queue->wait_take(i, 10000, complete) != S_OK ||
                complete.status != S_OK || complete.token != i ||
                complete.sequence != i ||
                complete.data.size() != kLargeRead ||
                !matches(complete.data, source,
                         (i * 3701u) % (source.size() - kLargeRead)))
                return false;
        }
        if (queue->outstanding_count() != 0 ||
            queue->reserved_bytes() != 0)
            return false;

        // Missing files and reads beyond EOF must report genuine
        // Win32 errors without inventing successful or partial data.
        if (queue->submit(
                {100, L"does_not_exist.bin", 0, 16}) != S_OK ||
            queue->submit(
                {101, L"generated.bin", source.size() - 8u, 16})
                != S_OK)
            return false;
        for (uint64_t id : {100ull, 101ull}) {
            HostFileReadCompletion complete;
            if (queue->wait_take(id, 10000, complete) != S_OK ||
                SUCCEEDED(complete.status) || !complete.data.empty())
                return false;
            if (id == 100 && complete.status !=
                    HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                return false;
            if (id == 101 && complete.status !=
                    HRESULT_FROM_WIN32(ERROR_HANDLE_EOF))
                return false;
        }
        if (queue->outstanding_count() || queue->reserved_bytes())
            return false;

        // Four real concurrent producers submit 32 independent reads
        // into one worker. No duplicate tokens, no race between
        // reservations and queue completions.
        std::atomic<bool> valid{true};
        std::vector<std::jthread> producers;
        for (uint64_t t = 0; t < 4; ++t) {
            producers.emplace_back([&, t] {
                for (uint64_t i = 0; i < 8; ++i) {
                    const uint64_t token = 200 + t*8 + i;
                    const uint64_t offset =
                        (token * 3911u) % (source.size() - 257u);
                    if (queue->submit(
                            {token, L"generated.bin", offset, 257})
                            != S_OK)
                        valid.store(false);
                }
            });
        }
        for (auto& producer : producers) producer.join();
        if (!valid.load() || queue->outstanding_count() != 32)
            return false;
        std::array<bool, 32> observed_sequence{};
        for (uint64_t i = 0; i < 32; ++i) {
            const uint64_t token = 200 + i;
            HostFileReadCompletion complete;
            if (queue->wait_take(token, 10000, complete) != S_OK ||
                complete.status != S_OK || complete.token != token ||
                complete.data.size() != 257 ||
                !matches(complete.data, source,
                         (token * 3911u) % (source.size() - 257u)))
                return false;
            // There were 66 earlier requests, so sequence range 67..98.
            if (complete.sequence < 67 || complete.sequence > 98 ||
                observed_sequence[complete.sequence - 67])
                return false;
            observed_sequence[complete.sequence - 67] = true;
        }
        if (!std::all_of(observed_sequence.begin(),
                         observed_sequence.end(), [](bool b){return b;}) ||
            queue->outstanding_count() || queue->reserved_bytes())
            return false;

        // Shutdown cancels queued requests, permits any already-running
        // file operation to finish, wakes waiters and rejects new work.
        for (uint64_t id = 400; id < 408; ++id)
            if (queue->submit({id, L"generated.bin", 0, kMiB}) != S_OK)
                return false;
        queue->shutdown();
        for (uint64_t id = 400; id < 408; ++id) {
            HostFileReadCompletion complete;
            if (queue->wait_take(id, 10000, complete) != S_OK ||
                complete.token != id ||
                (complete.status != S_OK &&
                 complete.status !=
                    HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED)) ||
                (complete.status == S_OK &&
                 (complete.data.size() != kMiB ||
                  !matches(complete.data, source, 0))) ||
                (complete.status != S_OK && !complete.data.empty()))
                return false;
        }
        if (queue->submit({900, L"generated.bin", 0, 1}) !=
                HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE) ||
            queue->outstanding_count() || queue->reserved_bytes() ||
            factory.object->SetStagingBufferSize(16*kMiB) != busy)
            return false;

        queue.reset();
        if (active_host_dstorage_queue_leases() != 0 ||
            factory.object->SetStagingBufferSize(16*kMiB) != S_OK ||
            host_dstorage_staging_bytes() != 16*kMiB ||
            factory.object->SetStagingBufferSize(0) != S_OK)
            return false;
        // A disk-backed host queue must never claim a working staging
        // reservation when the factory has a zero-byte staging pool.
        std::unique_ptr<HostAsyncFileQueue> no_stage;
        if (HostAsyncFileQueue::create(cleanup.root, no_stage) !=
                HRESULT_FROM_WIN32(ERROR_NOT_READY) ||
            no_stage || active_host_dstorage_queue_leases() != 0)
            return false;
        return factory.object->SetStagingBufferSize(32*kMiB) == S_OK &&
               host_dstorage_staging_bytes() == 32*kMiB;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::storage
