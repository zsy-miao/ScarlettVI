#include "dstorage_queue_host.h"
#include "dstorage_file_host.h"
#include "../runtime/physical_memory.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
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
struct AliasFixture {
    uint8_t* alias = nullptr;
    uintptr_t frame = 0;
    bool allocated = false;
    ~AliasFixture() {
        auto& mem = memory::PhysicalMemory::instance();
        if (alias) {
            (void)mem.map_pages(alias, 1, nullptr);
            (void)mem.release_alias_region(alias);
        }
        if (allocated) (void)mem.free_pages(1, &frame);
    }
    bool create() noexcept {
        auto& mem = memory::PhysicalMemory::instance();
        uintptr_t count = 1;
        if (!mem.allocate_pages(MEM_64K_PAGES, &count, &frame) ||
            count != 1) return false;
        allocated = true;
        alias = static_cast<uint8_t*>(
            mem.reserve_alias_region(nullptr, mem.page_bytes));
        return alias && mem.map_pages(alias, 1, &frame) == alias;
    }
};
struct TripleAliasFixture {
    static constexpr size_t count = 3;
    std::array<uintptr_t, count> frames{};
    uint8_t* alias = nullptr;
    bool allocated = false;
    ~TripleAliasFixture() {
        auto& memory = memory::PhysicalMemory::instance();
        if (alias) {
            (void)memory.map_pages(alias, count, nullptr);
            (void)memory.release_alias_region(alias);
        }
        if (allocated) (void)memory.free_pages(count, frames.data());
    }
    bool create() noexcept {
        auto& memory = memory::PhysicalMemory::instance();
        uintptr_t wanted = count;
        if (!memory.allocate_pages(
                MEM_64K_PAGES, &wanted, frames.data()) || wanted != count)
            return false;
        allocated = true;
        alias = static_cast<uint8_t*>(memory.reserve_alias_region(
            nullptr, count * memory.page_bytes));
        return alias && memory.map_pages(alias, count, frames.data()) == alias;
    }
};
struct FileFixture {
    fs::path root;
    fs::path file;
    fs::path nested;
    fs::path mounted_dir;
    fs::path mounted_file;
    bool created = false;
    ~FileFixture() {
        clear_dstorage_file_fixture_root();
        if (!created) return;
        std::error_code ec;
        if (!nested.empty()) {
            fs::remove(nested, ec);
            fs::remove(nested.parent_path(), ec);
            fs::remove(nested.parent_path().parent_path(), ec);
        }
        fs::remove(file, ec);
        fs::remove(root, ec);
        if (!mounted_file.empty()) fs::remove(mounted_file, ec);
        if (!mounted_dir.empty()) fs::remove(mounted_dir, ec);
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
        // Large enough for genuine file reads spanning three 64KiB aliases.
        std::vector<uint8_t> data(3 * 64 * 1024 + 8017);
        for (size_t i=0; i<data.size(); ++i)
            data[i] = static_cast<uint8_t>(((i*37u) ^ (i >> 4u) ^ 0x8bu) & 255u);
        {
            std::ofstream stream(cleanup.file, std::ios::binary);
            if (!stream ||
                !stream.write(reinterpret_cast<const char*>(data.data()),
                              static_cast<std::streamsize>(data.size())))
                return false;
        }
        // Generate a second identical file in a synthetic nested mount
        // to verify full queue readback, not merely a file-open handle.
        cleanup.nested = cleanup.root / L"packs" / L"batch" /
                         L"generated.bin";
        if (!fs::create_directories(cleanup.nested.parent_path()))
            return false;
        {
            std::ofstream stream(cleanup.nested, std::ios::binary);
            if (!stream ||
                !stream.write(reinterpret_cast<const char*>(data.data()),
                              static_cast<std::streamsize>(data.size())))
                return false;
        }
        // Independent generated virtual pack root, kept outside the
        // default fixture directory. The same file data permits the
        // existing exact-byte queue tests to exercise this mount.
        cleanup.mounted_dir = fs::path(L"reports") /
            (L"dstorage_queue_mount_" +
             std::to_wstring(GetCurrentProcessId()));
        if (fs::exists(cleanup.mounted_dir) ||
            !fs::create_directory(cleanup.mounted_dir)) return false;
        cleanup.mounted_file = cleanup.mounted_dir / L"generated.bin";
        {
            std::ofstream stream(cleanup.mounted_file, std::ios::binary);
            if (!stream ||
                !stream.write(reinterpret_cast<const char*>(data.data()),
                              static_cast<std::streamsize>(data.size())))
                return false;
        }
        if (configure_dstorage_file_fixture_root(cleanup.root) != S_OK ||
            configure_dstorage_fixture_mount(
                L"pack0", cleanup.mounted_dir) != S_OK)
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

        // Real queue reads can now originate from a ScarlettVI-owned
        // DirectStorage FILE COM object rather than a filename lookup.
        // ReOpenFile provides independent file positions across producers.
        HostDStorageFileX* source = nullptr;
        {
            // Real guest-owned UTF-16 XMem path, crossing a 64KiB page
            // boundary, goes through the actual COM OpenFile vtable.
            // After this scope the guest path mapping no longer exists,
            // yet the returned owned file COM object remains readable.
            TripleAliasFixture path_alias;
            if (!path_alias.create()) return false;
            const std::wstring nested_path = L"pack0:/generated.bin";
            auto* mapped_path = reinterpret_cast<wchar_t*>(
                path_alias.alias + memory::PhysicalMemory::page_bytes - 10);
            std::memcpy(mapped_path, nested_path.c_str(),
                        (nested_path.size() + 1) * sizeof(wchar_t));
            if (factory.ptr->OpenFile(
                    mapped_path, kObservedXboxDStorageFileIid,
                    reinterpret_cast<void**>(&source)) != S_OK ||
                !source) return false;
            std::memset(mapped_path, 0,
                        (nested_path.size() + 1) * sizeof(wchar_t));
        }
        // Raw guest EnqueueRequest (+0x18) now interprets the public Xbox
        // 64-byte struct ONLY through a host-approved immutable snapshot.
        // This proves true asynchronous destination writeback on generated
        // host memory while the normal title path remains fail-closed.
        auto destination = std::make_shared<std::vector<uint8_t>>(
            size_t{384}, uint8_t{0xcd});
        GuestDStorageRequest guest_read{};
        guest_read.destination = destination->data();
        guest_read.destination_size =
            static_cast<uint32_t>(destination->size());
        guest_read.file = source;
        guest_read.file_offset = 625;
        guest_read.source_size = guest_read.destination_size;
        if (register_host_dstorage_fixture_request(
                &guest_read, destination) != S_OK) {
            source->Release();
            return false;
        }
        if (register_host_dstorage_fixture_request(
                &guest_read, destination) !=
                HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
            unregister_host_dstorage_fixture_request(&guest_read);
            source->Release();
            return false;
        }
        enqueue_request(queue.ptr, &guest_read);
        // As documented by the GDK, the caller may reuse the request
        // structure immediately after EnqueueRequest returns. The worker
        // must rely solely on our approved immutable snapshot.
        guest_read.file = nullptr;
        guest_read.file_offset = UINT64_MAX;
        guest_read.destination = nullptr;
        enqueue_status(queue.ptr, status.ptr, 0);
        if (status.ptr->IsComplete(0) ||
            !std::all_of(destination->begin(), destination->end(),
                [](uint8_t byte) { return byte == 0xcd; })) {
            source->Release();
            return false;
        }
        submit(queue.ptr);
        if (!status_equals(status.ptr, 0, S_OK) ||
            !std::equal(destination->begin(), destination->end(),
                        data.begin()+625)) {
            source->Release();
            return false;
        }
        // A forged/unregistered pointer must never be dereferenced.
        enqueue_request(queue.ptr,
            reinterpret_cast<const void*>(uintptr_t{0x1234}));
        enqueue_status(queue.ptr, status.ptr, 0);
        submit(queue.ptr);
        if (!status_equals(status.ptr, 0, E_NOTIMPL)) {
            source->Release();
            return false;
        }
        // Real XMem physical CPU aliases are now eligible as normal
        // virtual-address destinations, but ONLY after host-only page
        // identity verification, pinning and a one-time request permit.
        AliasFixture pages;
        auto& physical = memory::PhysicalMemory::instance();
        const size_t original_pins = physical.pinned_alias_count();
        const size_t original_pages = physical.allocated_page_count();
        if (!pages.create()) {
            source->Release();
            return false;
        }
        constexpr size_t kAliasBytes = 512;
        uint8_t* const mapped_destination =
            pages.alias + memory::PhysicalMemory::page_bytes - kAliasBytes;
        std::fill_n(mapped_destination, kAliasBytes, uint8_t{0xac});
        GuestDStorageRequest alias_request{};
        alias_request.destination = mapped_destination;
        alias_request.destination_size = kAliasBytes;
        alias_request.file = source;
        alias_request.file_offset = 133;
        alias_request.source_size = kAliasBytes;
        // A frame-number mismatch fails without dereferencing destination.
        if (register_host_dstorage_fixture_alias_request(
                &alias_request, pages.frame + 16) == S_OK ||
            physical.pinned_alias_count() != original_pins) {
            source->Release();
            return false;
        }
        // A destination that straddles the end of the page is rejected.
        alias_request.destination = pages.alias +
            memory::PhysicalMemory::page_bytes - 128;
        if (register_host_dstorage_fixture_alias_request(
                &alias_request, pages.frame) == S_OK ||
            physical.pinned_alias_count() != original_pins) {
            source->Release();
            return false;
        }
        alias_request.destination = mapped_destination;
        if (register_host_dstorage_fixture_alias_request(
                &alias_request, pages.frame) != S_OK ||
            physical.pinned_alias_count() != original_pins + 1 ||
            physical.map_pages(pages.alias, 1, nullptr) != nullptr ||
            GetLastError() != ERROR_BUSY ||
            physical.release_alias_region(pages.alias) != FALSE ||
            GetLastError() != ERROR_BUSY) {
            source->Release();
            return false;
        }
        enqueue_request(queue.ptr, &alias_request);
        // Raw queue owns a snapshot; no reference to this caller's
        // stack request is needed after EnqueueRequest.
        alias_request.destination = nullptr;
        alias_request.file = nullptr;
        enqueue_status(queue.ptr, status.ptr, 0);
        if (status.ptr->IsComplete(0) ||
            physical.pinned_alias_count() != original_pins + 1 ||
            !std::all_of(mapped_destination,
                         mapped_destination + kAliasBytes,
                         [](uint8_t b) { return b == uint8_t{0xac}; })) {
            source->Release();
            return false;
        }
        submit(queue.ptr);
        if (!status_equals(status.ptr, 0, S_OK) ||
            !std::equal(mapped_destination,
                        mapped_destination + kAliasBytes,
                        data.begin() + 133) ||
            physical.pinned_alias_count() != original_pins) {
            source->Release();
            return false;
        }

        // Failed EOF reads must not write partial bytes, and must
        // release the alias pin before a later status marker completes.
        std::array<uint8_t, 64> baseline{};
        std::copy_n(mapped_destination + 7, baseline.size(),
                    baseline.begin());
        GuestDStorageRequest invalid_read{};
        invalid_read.destination = mapped_destination + 7;
        invalid_read.destination_size = static_cast<uint32_t>(baseline.size());
        invalid_read.file = source;
        invalid_read.file_offset = static_cast<uint64_t>(data.size()) + 17;
        invalid_read.source_size = invalid_read.destination_size;
        if (register_host_dstorage_fixture_alias_request(
                &invalid_read, pages.frame) != S_OK ||
            physical.pinned_alias_count() != original_pins + 1) {
            source->Release();
            return false;
        }
        enqueue_request(queue.ptr, &invalid_read);
        enqueue_status(queue.ptr, status.ptr, 0);
        submit(queue.ptr);
        if (!status_equals(status.ptr, 0,
                HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)) ||
            !std::equal(baseline.begin(), baseline.end(),
                        mapped_destination + 7) ||
            physical.pinned_alias_count() != original_pins) {
            source->Release();
            return false;
        }
        // A host-side discarded, unused permit also releases its pin.
        if (register_host_dstorage_fixture_alias_request(
                &invalid_read, pages.frame) != S_OK ||
            physical.pinned_alias_count() != original_pins + 1) {
            source->Release();
            return false;
        }
        unregister_host_dstorage_fixture_request(&invalid_read);
        if (physical.pinned_alias_count() != original_pins ||
            physical.map_pages(pages.alias, 1, nullptr) != pages.alias ||
            !physical.release_alias_region(pages.alias)) {
            source->Release();
            return false;
        }
        pages.alias = nullptr;
        if (!physical.free_pages(1, &pages.frame) ||
            physical.allocated_page_count() != original_pages) {
            source->Release();
            return false;
        }
        pages.allocated = false;

        // A single uncompressed request can now write across three
        // adjacent *virtual* CPU aliases (not an Xbox physical page list).
        // All three mapped frames must be independently pinned, and any
        // rejected middle page must unwind the already-acquired first pin.
        TripleAliasFixture triple;
        if (!triple.create()) {
            source->Release();
            return false;
        }
        constexpr size_t kMultiBytes =
            memory::PhysicalMemory::page_bytes + 2048;
        uint8_t* const multi_dest =
            triple.alias + memory::PhysicalMemory::page_bytes - 333;
        std::fill_n(multi_dest, kMultiBytes, uint8_t{0xa9});
        GuestDStorageRequest multi{};
        multi.file = source;
        multi.file_offset = 317;
        multi.destination = multi_dest;
        multi.destination_size = static_cast<uint32_t>(kMultiBytes);
        multi.source_size = multi.destination_size;
        std::array<uintptr_t, TripleAliasFixture::count> wrong_frames =
            triple.frames;
        wrong_frames[1] = triple.frames[0]; // first valid, middle wrong
        if (register_host_dstorage_fixture_alias_pages_request(
                &multi, wrong_frames.data(), wrong_frames.size()) == S_OK ||
            physical.pinned_alias_count() != original_pins ||
            register_host_dstorage_fixture_alias_pages_request(
                &multi, triple.frames.data(), 2) == S_OK ||
            physical.pinned_alias_count() != original_pins) {
            source->Release();
            return false;
        }

        // First page pins successfully, but an unmapped middle page
        // rejects the entire request and must not leak that first pin.
        uint8_t* const middle = triple.alias +
            memory::PhysicalMemory::page_bytes;
        if (physical.map_pages(middle, 1, nullptr) != middle ||
            register_host_dstorage_fixture_alias_pages_request(
                &multi, triple.frames.data(), triple.frames.size()) == S_OK ||
            physical.pinned_alias_count() != original_pins ||
            physical.map_pages(middle, 1, &triple.frames[1]) != middle) {
            source->Release();
            return false;
        }

        // Prevent wraparound before trying to pin a hostile virtual range.
        void* const saved_destination = multi.destination;
        multi.destination = reinterpret_cast<void*>(UINTPTR_MAX - 32);
        if (register_host_dstorage_fixture_alias_pages_request(
                &multi, triple.frames.data(), triple.frames.size()) == S_OK ||
            physical.pinned_alias_count() != original_pins) {
            source->Release();
            return false;
        }
        multi.destination = saved_destination;

        if (register_host_dstorage_fixture_alias_pages_request(
                &multi, triple.frames.data(), triple.frames.size()) != S_OK ||
            physical.pinned_alias_count() != original_pins + 3 ||
            physical.map_pages(middle, 1, nullptr) != nullptr ||
            GetLastError() != ERROR_BUSY ||
            physical.release_alias_region(triple.alias) != FALSE ||
            GetLastError() != ERROR_BUSY) {
            source->Release();
            return false;
        }
        enqueue_request(queue.ptr, &multi);
        multi.file = nullptr;
        multi.destination = nullptr; // request snapshot is immutable
        enqueue_status(queue.ptr, status.ptr, 0);
        if (status.ptr->IsComplete(0) ||
            physical.pinned_alias_count() != original_pins + 3 ||
            !std::all_of(multi_dest, multi_dest + kMultiBytes,
                [](uint8_t byte) { return byte == uint8_t{0xa9}; })) {
            source->Release();
            return false;
        }
        submit(queue.ptr);
        if (!status_equals(status.ptr, 0, S_OK) ||
            !std::equal(multi_dest, multi_dest + kMultiBytes,
                        data.begin() + 317) ||
            physical.pinned_alias_count() != original_pins) {
            source->Release();
            return false;
        }

        // Failed cross-page reads must leave *every* page untouched and
        // release all pins, including when the bytes run past file EOF.
        multi.destination = multi_dest;
        multi.file = source;
        multi.file_offset = static_cast<uint64_t>(data.size()) - 50;
        std::fill_n(multi_dest, kMultiBytes, uint8_t{0x79});
        if (register_host_dstorage_fixture_alias_pages_request(
                &multi, triple.frames.data(), triple.frames.size()) != S_OK ||
            physical.pinned_alias_count() != original_pins + 3) {
            source->Release();
            return false;
        }
        enqueue_request(queue.ptr, &multi);
        enqueue_status(queue.ptr, status.ptr, 0);
        submit(queue.ptr);
        if (!status_equals(status.ptr, 0,
                HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)) ||
            !std::all_of(multi_dest, multi_dest + kMultiBytes,
                [](uint8_t byte) { return byte == uint8_t{0x79}; }) ||
            physical.pinned_alias_count() != original_pins) {
            source->Release();
            return false;
        }

        // Real mapped guest CPU request memory (not a host-registered
        // pointer) can now be copied safely by the queue. Place the
        // 64-byte struct across two mapped XMem page slots to exercise
        // full-span ownership validation. Its destination spans three
        // independent mapped CPU aliases.
        TripleAliasFixture guest_request_pages;
        if (!guest_request_pages.create()) {
            source->Release();
            return false;
        }
        uint8_t* const guest_struct_bytes =
            guest_request_pages.alias +
            memory::PhysicalMemory::page_bytes - 20;
        GuestDStorageRequest resident{};
        resident.file = source;
        resident.destination = multi_dest;
        resident.destination_size = static_cast<uint32_t>(kMultiBytes);
        resident.source_size = resident.destination_size;
        resident.file_offset = 4091;
        std::memcpy(guest_struct_bytes, &resident, sizeof(resident));
        GuestDStorageRequest copied{};
        if (!physical.snapshot_mapped_alias(guest_struct_bytes,
                sizeof(copied), &copied) ||
            copied.destination != multi_dest ||
            physical.snapshot_mapped_alias(
                &resident, sizeof(resident), &copied)) {
            source->Release();
            return false;
        }
        // Unsupported guest option bits must not trigger writes.
        resident.options = 1;
        std::memcpy(guest_struct_bytes, &resident, sizeof(resident));
        enqueue_request(queue.ptr,
            reinterpret_cast<const GuestDStorageRequest*>(
                guest_struct_bytes));
        enqueue_status(queue.ptr, status.ptr, 0);
        submit(queue.ptr);
        if (!status_equals(status.ptr, 0, E_NOTIMPL) ||
            physical.pinned_alias_count() != original_pins) {
            source->Release();
            return false;
        }
        // An XMem-owned request pointing to non-XMem destination RAM
        // must fail closed, even when its source file COM is valid.
        resident.options = 0;
        std::array<uint8_t, 16> non_xmem{};
        resident.destination = non_xmem.data();
        resident.destination_size =
            static_cast<uint32_t>(non_xmem.size());
        resident.source_size = resident.destination_size;
        std::memcpy(guest_struct_bytes, &resident, sizeof(resident));
        enqueue_request(queue.ptr,
            reinterpret_cast<const GuestDStorageRequest*>(
                guest_struct_bytes));
        enqueue_status(queue.ptr, status.ptr, 0);
        submit(queue.ptr);
        if (!status_equals(status.ptr, 0, E_NOTIMPL) ||
            physical.pinned_alias_count() != original_pins) {
            source->Release();
            return false;
        }

        // Request snapshots must reject partially unmapped XMem ranges,
        // not just pointers that were never managed by ScarlettVI.
        uint8_t* const request_second_page =
            guest_request_pages.alias + memory::PhysicalMemory::page_bytes;
        if (physical.map_pages(request_second_page, 1, nullptr) !=
                request_second_page ||
            physical.snapshot_mapped_alias(
                guest_struct_bytes, sizeof(resident), &copied) ||
            physical.map_pages(request_second_page, 1,
                &guest_request_pages.frames[1]) != request_second_page) {
            source->Release();
            return false;
        }

        // Valid unregistered request uses ONLY XMem-owned source bytes.
        // The source struct can be wiped and its mappings released
        // immediately after enqueue; writeback retains DESTINATION pins.
        resident.destination = multi_dest;
        resident.destination_size = static_cast<uint32_t>(kMultiBytes);
        resident.source_size = resident.destination_size;
        std::fill_n(multi_dest, kMultiBytes, uint8_t{0x53});
        std::memcpy(guest_struct_bytes, &resident, sizeof(resident));
        enqueue_request(queue.ptr,
            reinterpret_cast<const GuestDStorageRequest*>(
                guest_struct_bytes));
        if (physical.pinned_alias_count() != original_pins + 3) {
            source->Release();
            return false;
        }
        std::memset(guest_struct_bytes, 0, sizeof(resident));
        if (physical.map_pages(guest_request_pages.alias,
                guest_request_pages.count, nullptr) !=
                guest_request_pages.alias ||
            !physical.release_alias_region(guest_request_pages.alias) ||
            !physical.free_pages(guest_request_pages.count,
                                 guest_request_pages.frames.data())) {
            source->Release();
            return false;
        }
        guest_request_pages.alias = nullptr;
        guest_request_pages.allocated = false;
        enqueue_status(queue.ptr, status.ptr, 0);
        if (status.ptr->IsComplete(0) ||
            !std::all_of(multi_dest, multi_dest + kMultiBytes,
                [](uint8_t b) { return b == uint8_t{0x53}; })) {
            source->Release();
            return false;
        }
        submit(queue.ptr);
        if (!status_equals(status.ptr, 0, S_OK) ||
            !std::equal(multi_dest, multi_dest + kMultiBytes,
                        data.begin() + 4091) ||
            physical.pinned_alias_count() != original_pins) {
            source->Release();
            return false;
        }

        // Reset the sentinel for the upcoming unsubmitted cancellation
        // test, which must not write any bytes before Submit.
        std::fill_n(multi_dest, kMultiBytes, uint8_t{0x79});

        // An unconsumed permission's destructor unpins the whole span.
        if (register_host_dstorage_fixture_alias_pages_request(
                &multi, triple.frames.data(), triple.frames.size()) != S_OK ||
            physical.pinned_alias_count() != original_pins + 3) {
            source->Release();
            return false;
        }
        unregister_host_dstorage_fixture_request(&multi);
        if (physical.pinned_alias_count() != original_pins) {
            source->Release();
            return false;
        }

        // A different queue closing with a still-unsubmitted multi-page
        // read must cancel the job and release all three held page pins.
        QueueOwner abandoned{};
        if (factory.ptr->CreateQueue1(&desc,
                kObservedXboxDStorageQueueIid,
                reinterpret_cast<void**>(&abandoned.ptr)) != S_OK ||
            !abandoned.ptr) {
            source->Release();
            return false;
        }
        multi.file_offset = 317;
        if (register_host_dstorage_fixture_alias_pages_request(
                &multi, triple.frames.data(), triple.frames.size()) != S_OK ||
            physical.pinned_alias_count() != original_pins + 3) {
            source->Release();
            return false;
        }
        abandoned.ptr->EnqueueRequest(&multi);
        abandoned.ptr->Close(); // no Submit: cancels held work
        if (physical.pinned_alias_count() != original_pins ||
            !std::all_of(multi_dest, multi_dest + kMultiBytes,
                [](uint8_t byte) { return byte == uint8_t{0x79}; })) {
            source->Release();
            return false;
        }
        abandoned.ptr->Release();
        abandoned.ptr = nullptr;
        if (live_host_dstorage_queues() != 1 ||
            active_host_dstorage_queue_leases() != 1 ||
            physical.map_pages(triple.alias, triple.count, nullptr) !=
                triple.alias ||
            !physical.release_alias_region(triple.alias) ||
            !physical.free_pages(triple.count, triple.frames.data()) ||
            physical.allocated_page_count() != original_pages) {
            source->Release();
            return false;
        }
        triple.alias = nullptr;
        triple.allocated = false;

        HANDLE invalid = reinterpret_cast<HANDLE>(uintptr_t{0x1234});
        if (reopen_owned_dstorage_file_for_read(
                reinterpret_cast<HostDStorageFileX*>(uintptr_t{0x1234}),
                &invalid) != E_INVALIDARG ||
            invalid != INVALID_HANDLE_VALUE) {
            source->Release();
            return false;
        }
        HANDLE original = source->GetHandle();
        if (original == INVALID_HANDLE_VALUE) {
            source->Release();
            return false;
        }
        LARGE_INTEGER position{};
        position.QuadPart = 1337;
        if (!SetFilePointerEx(original, position, nullptr, FILE_BEGIN)) {
            CloseHandle(original);
            source->Release();
            return false;
        }
        if (enqueue_host_dstorage_queue_fixture_file_read(
                queue.ptr, source, 10, 17, 256) != S_OK ||
            enqueue_host_dstorage_queue_fixture_file_read(
                queue.ptr, source, 11, 2048, 384) != S_OK) {
            CloseHandle(original);
            source->Release();
            return false;
        }
        // The independent queue read handles survive Close/Release of
        // their originating COM file; this does not expose title paths.
        source->Close();
        HANDLE closed = INVALID_HANDLE_VALUE;
        if (reopen_owned_dstorage_file_for_read(source, &closed) !=
                HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE) ||
            closed != INVALID_HANDLE_VALUE) {
            CloseHandle(original);
            source->Release();
            return false;
        }
        source->Release();
        source = nullptr;
        enqueue_status(queue.ptr, status.ptr, 0);
        if (status.ptr->IsComplete(0)) {
            CloseHandle(original);
            return false;
        }
        submit(queue.ptr);
        HostFileReadCompletion file_first, file_second;
        LARGE_INTEGER after{};
        const bool file_pass =
            status_equals(status.ptr, 0, S_OK) &&
            wait_host_dstorage_queue_fixture_read(
                queue.ptr, 10, 3000, file_first) == S_OK &&
            file_first.status == S_OK &&
            file_first.data.size() == 256 &&
            std::equal(file_first.data.begin(), file_first.data.end(),
                       data.begin() + 17) &&
            wait_host_dstorage_queue_fixture_read(
                queue.ptr, 11, 3000, file_second) == S_OK &&
            file_second.status == S_OK &&
            file_second.data.size() == 384 &&
            std::equal(file_second.data.begin(), file_second.data.end(),
                       data.begin() + 2048) &&
            SetFilePointerEx(original, LARGE_INTEGER{}, &after, FILE_CURRENT) &&
            after.QuadPart == 1337;
        CloseHandle(original);
        if (!file_pass) return false;

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

        // A long-lived guest-facing queue must NOT retain unconsumable
        // unsupported-operation tokens forever. Repeated submitted batches
        // of guest failures still admit status barriers and report errors.
        for (unsigned batch = 0; batch < 20; ++batch) {
            for (unsigned i = 0; i < 8; ++i)
                enqueue_request(queue.ptr, nullptr);
            enqueue_status(queue.ptr, status.ptr, 3);
            submit(queue.ptr);
            if (!status_equals(status.ptr, 3, E_NOTIMPL))
                return false;
        }

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
