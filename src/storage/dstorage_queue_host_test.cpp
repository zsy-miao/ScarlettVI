#include "dstorage_queue_host.h"
#include "dstorage_file_host.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace scarlett::storage {
namespace {
namespace fs = std::filesystem;
struct FactoryOwner {
    HostDStorageFactoryX2* ptr = nullptr;
    ~FactoryOwner() { if (ptr) ptr->Release(); }
};
struct QueueOwner {
    HostDStorageQueueX2* ptr = nullptr;
    ~QueueOwner() { if (ptr) ptr->Release(); }
};
struct StatusOwner {
    HostDStorageStatusArrayX* ptr = nullptr;
    ~StatusOwner() { if (ptr) ptr->Release(); }
};
struct FileFixture {
    fs::path root;
    fs::path file;
    bool created = false;
    ~FileFixture() {
        clear_dstorage_file_fixture_root();
        if (!created) return;
        std::error_code ec;
        fs::remove(file, ec);
        fs::remove(root, ec);
    }
};
using RawCreateQueue1 = HRESULT (STDMETHODCALLTYPE*)(
    HostDStorageFactoryX2*, const void*, REFIID, void**) noexcept;
using RawEnqueueStatus = void (STDMETHODCALLTYPE*)(
    HostDStorageQueueX2*, HostDStorageStatusArrayX*, uint32_t) noexcept;
using RawEnqueueRequest = void (STDMETHODCALLTYPE*)(
    HostDStorageQueueX2*, const void*) noexcept;
using RawSubmit = void (STDMETHODCALLTYPE*)(
    HostDStorageQueueX2*) noexcept;
using RawClose = void (STDMETHODCALLTYPE*)(
    HostDStorageQueueX2*) noexcept;
using RawGetEvent = HANDLE (STDMETHODCALLTYPE*)(
    HostDStorageQueueX2*) noexcept;

bool status_equals(HostDStorageStatusArrayX* array, uint32_t index,
                   HRESULT expected) {
    for (unsigned i=0; i<3000; ++i) {
        if (array->IsComplete(index))
            return array->GetHResult(index) == expected;
        Sleep(1);
    }
    return false;
}
} // namespace

bool verify_dstorage_queue_com_fixture() noexcept {
    try {
        if (live_host_dstorage_queues() ||
            active_host_dstorage_queue_leases()) return false;
        FactoryOwner factory{};
        if (DStorageGetFactory(kObservedXboxDStorageFactoryIid,
                reinterpret_cast<void**>(&factory.ptr)) != S_OK ||
            !factory.ptr || factory.ptr->SetStagingBufferSize(
                kDefaultStagingBytes) != S_OK)
            return false;

        GuestDStorageQueueDesc1 desc{0, 64, 0, "generated", 1};
        void* output = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (factory.ptr->CreateQueue1(&desc,
                kObservedXboxDStorageQueueIid, &output) != E_NOTIMPL ||
            output) return false;

        FileFixture cleanup;
        fs::create_directories(L"reports");
        cleanup.root = fs::path(L"reports") /
            (L"dstorage_queue_fixture_" +
             std::to_wstring(GetCurrentProcessId()));
        if (fs::exists(cleanup.root) ||
            !fs::create_directory(cleanup.root)) return false;
        cleanup.created = true;
        cleanup.file = cleanup.root / L"generated.bin";
        std::vector<uint8_t> data(4096+37);
        for (size_t i=0; i<data.size(); ++i)
            data[i] = static_cast<uint8_t>(((i*37u) ^ (i >> 4u) ^ 0x8bu) & 255u);
        {
            std::ofstream stream(cleanup.file, std::ios::binary);
            if (!stream ||
                !stream.write(reinterpret_cast<const char*>(data.data()),
                              static_cast<std::streamsize>(data.size())))
                return false;
        }
        if (configure_dstorage_file_fixture_root(cleanup.root) != S_OK)
            return false;
        const GUID wrong{0x784934fa,0x23c0,0x1234,{1,2,3,4,5,6,7,8}};
        output = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (factory.ptr->CreateQueue1(
                &desc, wrong, &output) != E_NOINTERFACE || output ||
            factory.ptr->CreateQueue1(&desc,
                kObservedXboxDStorageQueueIid, nullptr) != E_POINTER)
            return false;
        for (GuestDStorageQueueDesc1 invalid : std::array{
                 GuestDStorageQueueDesc1{1,64,0,nullptr,1},
                 GuestDStorageQueueDesc1{0,0,0,nullptr,1},
                 GuestDStorageQueueDesc1{0,65,0,nullptr,1},
                 GuestDStorageQueueDesc1{0,64,3,nullptr,1},
                 GuestDStorageQueueDesc1{0,64,0,nullptr,2}}) {
            output = reinterpret_cast<void*>(uintptr_t{0x1234});
            if (factory.ptr->CreateQueue1(&invalid,
                    kObservedXboxDStorageQueueIid, &output) != E_INVALIDARG ||
                output) return false;
        }

        auto** factory_vtable = *reinterpret_cast<void***>(factory.ptr);
        auto create_raw = reinterpret_cast<RawCreateQueue1>(factory_vtable[10]);
        QueueOwner queue{};
        if (!create_raw || create_raw(factory.ptr, &desc,
                kObservedXboxDStorageQueueIid,
                reinterpret_cast<void**>(&queue.ptr)) != S_OK ||
            !queue.ptr || live_host_dstorage_queues() != 1 ||
            active_host_dstorage_queue_leases() != 1)
            return false;
        if (factory.ptr->SetStagingBufferSize(
                kDefaultStagingBytes) != HRESULT_FROM_WIN32(ERROR_BUSY))
            return false;
        IUnknown* unknown = nullptr;
        if (queue.ptr->QueryInterface(IID_IUnknown,
                reinterpret_cast<void**>(&unknown)) != S_OK ||
            unknown != static_cast<IUnknown*>(queue.ptr))
            return false;
        unknown->Release();
        output = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (queue.ptr->QueryInterface(wrong, &output) != E_NOINTERFACE ||
            output || queue.ptr->QueryInterface(IID_IUnknown, nullptr) != E_POINTER)
            return false;
        auto** vt = *reinterpret_cast<void***>(queue.ptr);
        const auto enqueue_status =
            reinterpret_cast<RawEnqueueStatus>(vt[4]);
        const auto enqueue_request =
            reinterpret_cast<RawEnqueueRequest>(vt[3]);
        const auto submit = reinterpret_cast<RawSubmit>(vt[6]);
        const auto close = reinterpret_cast<RawClose>(vt[8]);
        const auto get_event = reinterpret_cast<RawGetEvent>(vt[9]);
        if (!enqueue_status || !enqueue_request || !submit ||
            !close || !get_event || !get_event(queue.ptr) ||
            WaitForSingleObject(get_event(queue.ptr), 0) != WAIT_TIMEOUT)
            return false;

        StatusOwner status{};
        if (factory.ptr->CreateStatusArray(4, "generated",
                kObservedXboxDStorageStatusIid,
                reinterpret_cast<void**>(&status.ptr)) != S_OK ||
            !status.ptr) return false;

        // A real read and its marker stay pending until raw guest Submit.
        if (enqueue_host_dstorage_queue_fixture_read(queue.ptr,
                {1, L"generated.bin", 3, 1024}) != S_OK)
            return false;
        enqueue_status(queue.ptr, status.ptr, 0);
        HostFileReadCompletion first;
        if (status.ptr->IsComplete(0) ||
            wait_host_dstorage_queue_fixture_read(
                queue.ptr, 1, 0, first) != HRESULT_FROM_WIN32(WAIT_TIMEOUT))
            return false;
        submit(queue.ptr);
        if (!status_equals(status.ptr, 0, S_OK) ||
            wait_host_dstorage_queue_fixture_read(
                queue.ptr, 1, 3000, first) != S_OK ||
            first.status != S_OK || first.data.size() != 1024 ||
            !std::equal(first.data.begin(), first.data.end(),
                        data.begin()+3))
            return false;

        // Unsupported guest request is never reported as successful.
        enqueue_request(queue.ptr, nullptr); // deliberately no structure dereference
        enqueue_status(queue.ptr, status.ptr, 1);
        if (status.ptr->IsComplete(1) ||
            last_host_dstorage_queue_error(queue.ptr) != E_NOTIMPL ||
            WaitForSingleObject(get_event(queue.ptr), 0) != WAIT_OBJECT_0)
            return false;
        submit(queue.ptr);
        if (!status_equals(status.ptr, 1, E_NOTIMPL))
            return false;
        // Untrusted or unrelated interface pointers are checked by
        // address against our live COM registry before any dereference.
        enqueue_status(queue.ptr,
            reinterpret_cast<HostDStorageStatusArrayX*>(uintptr_t{0x1234}),
            0);
        if (status.ptr->GetHResult(0) != S_OK) return false;

        // A genuinely failed file read is the FIRST failure when a
        // following unsupported request also fails in the same segment.
        if (enqueue_host_dstorage_queue_fixture_read(queue.ptr,
                {2, L"missing.bin", 0, 16}) != S_OK)
            return false;
        enqueue_request(queue.ptr, nullptr);
        enqueue_status(queue.ptr, status.ptr, 2);
        submit(queue.ptr);
        HostFileReadCompletion missing;
        if (!status_equals(status.ptr, 2,
                HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) ||
            wait_host_dstorage_queue_fixture_read(
                queue.ptr, 2, 3000, missing) != S_OK ||
            missing.status != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            return false;

        // The first-error aggregate resets at the next status barrier.
        if (enqueue_host_dstorage_queue_fixture_read(queue.ptr,
                {3, L"generated.bin", 2048, 256}) != S_OK)
            return false;
        enqueue_status(queue.ptr, status.ptr, 3);
        submit(queue.ptr);
        HostFileReadCompletion final_read;
        if (!status_equals(status.ptr, 3, S_OK) ||
            wait_host_dstorage_queue_fixture_read(
                queue.ptr, 3, 3000, final_read) != S_OK ||
            final_read.status != S_OK ||
            !std::equal(final_read.data.begin(), final_read.data.end(),
                        data.begin()+2048))
            return false;

        // Avoid writing an unknown-sized Xbox struct; errors remain
        // observable via a real Win32 event and the host-only diagnostic.
        std::array<uint8_t, 96> guard{};
        guard.fill(0xab);
        queue.ptr->Query1(guard.data());
        queue.ptr->RetrieveErrorRecord(guard.data());
        for (auto byte : guard) if (byte != 0xab) return false;

        close(queue.ptr);
        close(queue.ptr);
        if (enqueue_host_dstorage_queue_fixture_read(queue.ptr,
                {4, L"generated.bin", 0, 32}) !=
                HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE) ||
            factory.ptr->SetStagingBufferSize(kDefaultStagingBytes) !=
                HRESULT_FROM_WIN32(ERROR_BUSY))
            return false;
        queue.ptr->Release();
        queue.ptr = nullptr;
        if (live_host_dstorage_queues() ||
            active_host_dstorage_queue_leases() ||
            factory.ptr->SetStagingBufferSize(kDefaultStagingBytes) != S_OK)
            return false;
        clear_dstorage_file_fixture_root();
        output = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (factory.ptr->CreateQueue1(&desc,
                kObservedXboxDStorageQueueIid, &output) != E_NOTIMPL || output)
            return false;
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::storage
