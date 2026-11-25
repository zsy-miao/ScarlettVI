#include "dstorage_status_host.h"
#include "dstorage_factory_host.h"
#include "dstorage_async_queue.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <vector>

namespace scarlett::storage {
namespace {
namespace fs = std::filesystem;
using CreateStatusSlot = HRESULT (STDMETHODCALLTYPE*)(
    HostDStorageFactoryX2*, uint32_t, const char*, REFIID, void**) noexcept;
using OpenFileSlot = HRESULT (STDMETHODCALLTYPE*)(
    HostDStorageFactoryX2*, const wchar_t*, REFIID, void**) noexcept;
using GetStatusSlot = HRESULT (STDMETHODCALLTYPE*)(
    HostDStorageStatusArrayX*, uint32_t) noexcept;
using IsCompleteSlot = bool (STDMETHODCALLTYPE*)(
    HostDStorageStatusArrayX*, uint32_t) noexcept;

struct FactoryScope {
    HostDStorageFactoryX2* value = nullptr;
    ~FactoryScope() { if (value) value->Release(); }
};
struct StatusScope {
    HostDStorageStatusArrayX* value = nullptr;
    ~StatusScope() { if (value) value->Release(); }
};
struct GeneratedRoot {
    fs::path root;
    fs::path file;
    bool owned = false;
    ~GeneratedRoot() {
        if (!owned) return;
        std::error_code ec;
        fs::remove(file, ec);
        fs::remove(root, ec);
    }
};
constexpr HRESULT kMissing = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
constexpr HRESULT kAborted = HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);

bool has_bytes(const HostFileReadCompletion& completion,
               std::span<const uint8_t> source,
               uint64_t token, uint64_t sequence,
               uint64_t offset, uint32_t length) noexcept {
    return completion.token == token &&
           completion.sequence == sequence &&
           completion.status == S_OK &&
           completion.data.size() == length &&
           std::equal(completion.data.begin(), completion.data.end(),
                      source.data() + offset);
}
} // namespace

bool verify_dstorage_status_array_fixture() noexcept {
    try {
        const uint32_t before = live_host_dstorage_status_arrays();
        const GUID invalid{0x2191390a,0x1234,0x4321,{1,2,3,4,5,6,7,8}};
        FactoryScope factory;
        if (DStorageGetFactory(kObservedXboxDStorageFactoryIid,
                reinterpret_cast<void**>(&factory.value)) != S_OK ||
            !factory.value) return false;

        // Actual caller evidence overrides the alphabetical method
        // listing: +0x20 is OpenFile; +0x28 is CreateStatusArray.
        // Raw vtable dispatch catches accidental reordering that
        // type-checked C++ factory calls would otherwise miss.
        auto* factory_table = *reinterpret_cast<void***>(factory.value);
        const auto open_file = reinterpret_cast<OpenFileSlot>(
            factory_table[4]);
        const auto create = reinterpret_cast<CreateStatusSlot>(
            factory_table[5]);
        if (!create || !open_file) return false;
        void* unknown = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (open_file(factory.value, L"generated_unopened.bin",
                      kObservedXboxDStorageFileIid, &unknown) !=
                E_NOTIMPL || unknown)
            return false;
        unknown = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (create(factory.value, 4, "generated", invalid,
                   &unknown) != E_NOINTERFACE || unknown ||
            create(factory.value, 4, nullptr,
                   kObservedXboxDStorageStatusIid,
                   nullptr) != E_POINTER)
            return false;
        for (uint32_t capacity : {0u,257u,0xffffffffu}) {
            unknown = reinterpret_cast<void*>(uintptr_t{0x1234});
            if (create(factory.value, capacity, "generated",
                       kObservedXboxDStorageStatusIid,
                       &unknown) != E_INVALIDARG || unknown)
                return false;
        }

        StatusScope status;
        if (create(factory.value, 4, "generated",
                   kObservedXboxDStorageStatusIid,
                   reinterpret_cast<void**>(&status.value)) != S_OK ||
            !status.value ||
            live_host_dstorage_status_arrays() != before + 1u)
            return false;
        auto* slots = *reinterpret_cast<void***>(status.value);
        // Windows DirectStorage vtable order corroborates Xbox method
        // names, but actual Xbox status-array callsites need more proof.
        const auto done = reinterpret_cast<IsCompleteSlot>(slots[3]);
        const auto get = reinterpret_cast<GetStatusSlot>(slots[4]);
        if (!get || !done ||
            get(status.value, 0) != E_PENDING ||
            done(status.value, 0) ||
            get(status.value, 4) != E_INVALIDARG ||
            done(status.value, 4))
            return false;
        IUnknown* ref = nullptr;
        if (status.value->QueryInterface(
                IID_IUnknown, reinterpret_cast<void**>(&ref)) != S_OK ||
            !ref || ref != static_cast<IUnknown*>(status.value))
            return false;
        ref->Release();
        unknown = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (status.value->QueryInterface(invalid, &unknown) !=
                E_NOINTERFACE || unknown)
            return false;

        GeneratedRoot generated;
        fs::create_directories(L"reports");
        generated.root = fs::path(L"reports") /
            (L"dstorage_status_fixture_" +
             std::to_wstring(GetCurrentProcessId()));
        if (fs::exists(generated.root) ||
            !fs::create_directory(generated.root)) return false;
        generated.owned = true;
        generated.file = generated.root / L"status_data.bin";
        std::vector<uint8_t> bytes(4096);
        for (size_t i=0;i<bytes.size();++i)
            bytes[i] = static_cast<uint8_t>(
                ((i*73u) ^ (i>>3u) ^ 0x6bu) & 0xff);
        {
            std::ofstream output(generated.file, std::ios::binary);
            if (!output ||
                !output.write(
                    reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size())))
                return false;
        }

        std::unique_ptr<HostAsyncFileQueue> queue;
        if (HostAsyncFileQueue::create(
                generated.root, queue,
                HostAsyncFileQueue::SubmitPolicy::Manual) != S_OK ||
            !queue) return false;

        // Invalid index or missing status pointer must fail before
        // reserving any queue slot or modifying any completion state.
        if (queue->enqueue_status(97, status.value, 4) != E_INVALIDARG ||
            queue->enqueue_status(98, nullptr, 0) != E_POINTER ||
            queue->outstanding_count() || queue->reserved_bytes())
            return false;

        // Two successes separated by an error. Each enqueued status
        // becomes pending *without IO* and reports the first actual
        // worker failure in its own request interval.
        if (queue->enqueue({1,L"status_data.bin",0,128}) != S_OK ||
            queue->enqueue_status(2,status.value,0) != S_OK ||
            queue->enqueue_status(99,status.value,0) !=
                HRESULT_FROM_WIN32(ERROR_BUSY) ||
            queue->enqueue({3,L"missing.bin",0,64}) != S_OK ||
            queue->enqueue_status(4,status.value,1) != S_OK ||
            queue->enqueue({5,L"status_data.bin",256,64}) != S_OK ||
            queue->enqueue_status(6,status.value,2) != S_OK ||
            queue->pending_submission_count() != 6 ||
            get(status.value,0) != E_PENDING ||
            get(status.value,1) != E_PENDING ||
            get(status.value,2) != E_PENDING ||
            done(status.value,0) ||
            queue->submit_pending() != 6)
            return false;

        for (uint64_t token=1;token<=6;++token) {
            HostFileReadCompletion completed;
            if (queue->wait_take(token,10000,completed) != S_OK ||
                completed.token != token ||
                completed.sequence != token)
                return false;
            if (token==1 && !has_bytes(completed,bytes,1,1,0,128))
                return false;
            if (token==3 &&
                (completed.status != kMissing ||
                 !completed.data.empty())) return false;
            if (token==5 && !has_bytes(completed,bytes,5,5,256,64))
                return false;
            if (token%2==0) {
                const HRESULT expected = token==4 ? kMissing : S_OK;
                if (completed.status != expected ||
                    !completed.data.empty())
                    return false;
            }
        }
        if (!done(status.value,0) ||
            !done(status.value,1) ||
            !done(status.value,2) ||
            get(status.value,0) != S_OK ||
            get(status.value,1) != kMissing ||
            get(status.value,2) != S_OK ||
            queue->outstanding_count() ||
            queue->reserved_bytes())
            return false;

        // Reuse a completed slot, cancel before submit. It must become
        // ERROR_OPERATION_ABORTED, not a fabricated successful read.
        if (queue->enqueue_status(7,status.value,1) != S_OK ||
            get(status.value,1) != E_PENDING ||
            done(status.value,1) ||
            queue->cancel_unsubmitted(7) != S_OK)
            return false;
        HostFileReadCompletion canceled;
        if (queue->wait_take(7,0,canceled) != S_OK ||
            canceled.status != kAborted ||
            !canceled.data.empty() ||
            get(status.value,1) != kAborted ||
            !done(status.value,1))
            return false;

        // Outstanding marker holds a genuine COM reference even when
        // the original title-like owner releases its own status object.
        StatusScope temporary;
        if (create(factory.value, 1, "lifetime",
                   kObservedXboxDStorageStatusIid,
                   reinterpret_cast<void**>(&temporary.value)) != S_OK ||
            live_host_dstorage_status_arrays() != before+2)
            return false;
        if (queue->enqueue({8,L"status_data.bin",100,40}) != S_OK ||
            queue->enqueue_status(9,temporary.value,0) != S_OK)
            return false;
        temporary.value->Release();
        temporary.value = nullptr;
        if (live_host_dstorage_status_arrays() != before+2 ||
            queue->submit_pending() != 2)
            return false;
        HostFileReadCompletion read, barrier;
        if (queue->wait_take(8,10000,read) != S_OK ||
            !has_bytes(read,bytes,8,8,100,40) ||
            queue->wait_take(9,10000,barrier) != S_OK ||
            barrier.status != S_OK ||
            live_host_dstorage_status_arrays() != before+1)
            return false;

        queue.reset();
        status.value->Release();
        status.value = nullptr;
        return live_host_dstorage_status_arrays() == before &&
               active_host_dstorage_queue_leases() == 0;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::storage
