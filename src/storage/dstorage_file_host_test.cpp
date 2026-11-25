#include "dstorage_file_host.h"
#include "../runtime/physical_memory.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace scarlett::storage {
namespace {
namespace fs = std::filesystem;
constexpr uint32_t kMiB = 1024u * 1024u;
struct FactoryOwner {
    HostDStorageFactoryX2* p = nullptr;
    ~FactoryOwner() { if (p) p->Release(); }
};
struct FileOwner {
    HostDStorageFileX* p = nullptr;
    ~FileOwner() { if (p) p->Release(); }
};
struct FixtureRoot {
    fs::path dir;
    fs::path file;
    fs::path nested_dir;
    fs::path nested_file;
    fs::path symbolic_directory;
    fs::path outside_file;
    bool created = false;
    ~FixtureRoot() {
        clear_dstorage_file_fixture_root();
        if (created) {
            std::error_code error;
            if (!symbolic_directory.empty())
                fs::remove(symbolic_directory, error);
            if (!nested_file.empty()) fs::remove(nested_file, error);
            if (!nested_dir.empty()) {
                fs::remove(nested_dir, error);
                fs::remove(nested_dir.parent_path(), error);
            }
            fs::remove(file, error);
            fs::remove(dir, error);
            if (!outside_file.empty()) fs::remove(outside_file, error);
        }
    }
};
using OpenFileFn = HRESULT (STDMETHODCALLTYPE*)(
    HostDStorageFactoryX2*, const wchar_t*, REFIID, void**) noexcept;

// Native literals are admitted to the COM bridge only by an explicit
// host-test permit. This wrapper is NOT a title API and passes a trusted
// snapshot, not permission to dereference the raw pointer.
HRESULT fixture_open(HostDStorageFactoryX2* factory,
                     const wchar_t* source, REFIID iid, void** out) {
    if (source) {
        const HRESULT approved = register_host_dstorage_fixture_path(
            source, std::wstring(source));
        if (FAILED(approved)) return approved;
    }
    const HRESULT result = factory->OpenFile(source, iid, out);
    clear_host_dstorage_fixture_path();
    return result;
}
struct TwoMappedPathPages {
    uintptr_t frames[2]{};
    uint8_t* alias = nullptr;
    bool allocated = false;
    bool create() noexcept {
        auto& memory = memory::PhysicalMemory::instance();
        uintptr_t count = 2;
        if (!memory.allocate_pages(MEM_64K_PAGES, &count, frames) ||
            count != 2) return false;
        allocated = true;
        alias = static_cast<uint8_t*>(memory.reserve_alias_region(
            nullptr, 2 * memory.page_bytes));
        return alias && memory.map_pages(alias, 2, frames) == alias;
    }
    ~TwoMappedPathPages() {
        auto& memory = memory::PhysicalMemory::instance();
        if (alias) {
            (void)memory.map_pages(alias, 2, nullptr);
            (void)memory.release_alias_region(alias);
        }
        if (allocated) (void)memory.free_pages(2, frames);
    }
};
} // namespace

bool verify_dstorage_file_fixture() noexcept {
    try {
        if (live_host_dstorage_files() ||
            active_host_dstorage_file_leases()) return false;
        FactoryOwner factory{};
        if (DStorageGetFactory(kObservedXboxDStorageFactoryIid,
                reinterpret_cast<void**>(&factory.p)) != S_OK ||
            !factory.p || factory.p->SetStagingBufferSize(32*kMiB) != S_OK)
            return false;

        // The file service must remain disabled until a trusted test root
        // is explicitly configured, and must clear its out pointer.
        void* out = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (fixture_open(factory.p, L"generated.bin",
                kObservedXboxDStorageFileIid, &out) != E_NOTIMPL ||
            out != nullptr) return false;

        FixtureRoot cleanup;
        fs::create_directories(L"reports");
        cleanup.dir = fs::path(L"reports") /
            (L"dstorage_file_fixture_" + std::to_wstring(GetCurrentProcessId()));
        if (fs::exists(cleanup.dir) ||
            !fs::create_directory(cleanup.dir)) return false;
        cleanup.created = true;
        cleanup.file = cleanup.dir / L"generated.bin";
        std::vector<uint8_t> expected(8192+37);
        for (size_t i=0; i<expected.size(); ++i)
            expected[i] = static_cast<uint8_t>(
                ((i * 71u) ^ (i >> 3u) ^ 0x5du) & 0xffu);
        {
            std::ofstream stream(cleanup.file, std::ios::binary);
            if (!stream ||
                !stream.write(reinterpret_cast<const char*>(expected.data()),
                              static_cast<std::streamsize>(expected.size())))
                return false;
        }
        // Generated nested asset directory: no title assets are mounted.
        cleanup.nested_dir = cleanup.dir / L"packs" / L"level1";
        if (!fs::create_directories(cleanup.nested_dir)) return false;
        cleanup.nested_file = cleanup.nested_dir / L"sample.bin";
        {
            std::ofstream stream(cleanup.nested_file, std::ios::binary);
            if (!stream ||
                !stream.write(reinterpret_cast<const char*>(expected.data()),
                              static_cast<std::streamsize>(expected.size())))
                return false;
        }
        if (configure_dstorage_file_fixture_root(cleanup.dir) != S_OK ||
            configure_dstorage_file_fixture_root(cleanup.dir) !=
                HRESULT_FROM_WIN32(ERROR_BUSY)) return false;
        if (fixture_open(factory.p, L"generated.bin",
                kObservedXboxDStorageFileIid, nullptr) != E_POINTER)
            return false;

        const GUID invalid{0x4e934ab6,0x67ac,0x1234,{1,2,3,4,5,6,7,8}};
        out = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (fixture_open(factory.p, L"generated.bin", invalid, &out) !=
                E_NOINTERFACE || out) return false;
        constexpr std::array<const wchar_t*, 25> bad{
            L"", L".", L"..", L"..\\generated.bin", L"../generated.bin",
            L"C:generated.bin", L"CON.bin", L"foo.", L"foo bar.bin",
            L"generated.bin:secret", L"\\generated.bin", L"/generated.bin",
            L"//server/share", L"packs//level1/sample.bin",
            L"packs\\\\level1\\sample.bin", L"packs\\", L"packs/",
            L"packs\\.\\level1\\sample.bin", L"packs\\..\\generated.bin",
            L"packs\\CON.txt", L"packs\\lpt0.txt",
            L"packs\\level1\\AUX.txt", L"packs\\level1\\foo.",
            L"packs\\level1\\name:stream", L"packs\\level1\\foo bar"};
        for (const wchar_t* path : bad) {
            out = reinterpret_cast<void*>(uintptr_t{0x1234});
            if (fixture_open(factory.p, path, kObservedXboxDStorageFileIid,
                    &out) != E_INVALIDARG || out) return false;
        }
        out = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (fixture_open(factory.p, L"absent.bin",
                kObservedXboxDStorageFileIid, &out) !=
                HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || out) return false;

        // The fixture VFS accepts bounded nested slash/backslash paths,
        // but it does not permit drive syntax, traversal or device names.
        for (const wchar_t* guest_path :
             {L"packs\\level1\\sample.bin", L"packs/level1/sample.bin",
              L"packs\\level1/sample.bin"}) {
            FileOwner nested{};
            if (fixture_open(factory.p,
                    guest_path, kObservedXboxDStorageFileIid,
                    reinterpret_cast<void**>(&nested.p)) != S_OK ||
                !nested.p) return false;
            HANDLE h = nested.p->GetHandle();
            if (h == INVALID_HANDLE_VALUE) return false;
            std::vector<uint8_t> bytes(expected.size());
            DWORD got = 0;
            const bool exact = ReadFile(
                    h, bytes.data(), static_cast<DWORD>(bytes.size()),
                    &got, nullptr) &&
                got == bytes.size() && bytes == expected;
            CloseHandle(h);
            if (!exact) return false;
        }
        out = reinterpret_cast<void*>(uintptr_t{0x1234});
        const std::wstring too_deep = L"a\\b\\c\\d\\e\\f\\g\\h\\i.bin";
        if (fixture_open(factory.p, too_deep.c_str(),
                kObservedXboxDStorageFileIid, &out) != E_INVALIDARG ||
            out) return false;
        out = reinterpret_cast<void*>(uintptr_t{0x1234});
        const std::wstring too_long(241, L'a');
        if (fixture_open(factory.p, too_long.c_str(),
                kObservedXboxDStorageFileIid, &out) != E_INVALIDARG ||
            out) return false;

        // A normal COM OpenFile call may now accept a path stored entirely
        // inside XMem CPU aliases, with no special host registration.
        // Position it 12 bytes before a 64KiB boundary: both pages are
        // required even though the final text is only a few dozen chars.
        const size_t old_guest_pages =
            memory::PhysicalMemory::instance().allocated_page_count();
        const size_t old_guest_pins =
            memory::PhysicalMemory::instance().pinned_alias_count();
        {
            auto& mem = memory::PhysicalMemory::instance();
            TwoMappedPathPages owned_pages;
            if (!owned_pages.create()) return false;
            auto* const guest_path = reinterpret_cast<wchar_t*>(
                owned_pages.alias + mem.page_bytes - 12);
            const std::wstring nested_name = L"packs\\level1\\sample.bin";
            std::memcpy(guest_path, nested_name.c_str(),
                        (nested_name.size() + 1) * sizeof(wchar_t));
            std::wstring copied = L"original";
            if (!mem.snapshot_mapped_alias_utf16(
                    guest_path, 240, copied) || copied != nested_name)
                return false;
            FileOwner guest_file{};
            if (factory.p->OpenFile(
                    guest_path, kObservedXboxDStorageFileIid,
                    reinterpret_cast<void**>(&guest_file.p)) != S_OK ||
                !guest_file.p) return false;

            // The file object owns its independent handle; the request
            // path itself may be immediately reused or unmapped.
            std::memset(guest_path, 0, (nested_name.size()+1)*sizeof(wchar_t));
            HANDLE borrowed = guest_file.p->GetHandle();
            if (borrowed == INVALID_HANDLE_VALUE) return false;
            std::vector<uint8_t> readback(expected.size());
            DWORD read_bytes = 0;
            const bool accurate = ReadFile(
                borrowed, readback.data(),
                static_cast<DWORD>(readback.size()),
                &read_bytes, nullptr) &&
                read_bytes == readback.size() && readback == expected;
            CloseHandle(borrowed);
            guest_file.p->Release();
            guest_file.p = nullptr;
            if (!accurate) return false;

            // Unregistered native/invalid pointers are never inspected.
            out = reinterpret_cast<void*>(uintptr_t{0x1234});
            if (factory.p->OpenFile(
                    reinterpret_cast<const wchar_t*>(uintptr_t{0x1234}),
                    kObservedXboxDStorageFileIid, &out) != E_INVALIDARG ||
                out) return false;
            out = reinterpret_cast<void*>(uintptr_t{0x1234});
            if (factory.p->OpenFile(
                    L"generated.bin", kObservedXboxDStorageFileIid,
                    &out) != E_INVALIDARG || out) return false;

            // One-shot host fixture permits are bound to exact pointer
            // identity and are consumed after one COM OpenFile call.
            constexpr const wchar_t* kNativeLiteral = L"generated.bin";
            if (register_host_dstorage_fixture_path(
                    kNativeLiteral, L"generated.bin") != S_OK ||
                register_host_dstorage_fixture_path(
                    kNativeLiteral, L"generated.bin") !=
                    HRESULT_FROM_WIN32(ERROR_BUSY)) return false;
            FileOwner only_once{};
            if (factory.p->OpenFile(
                    kNativeLiteral, kObservedXboxDStorageFileIid,
                    reinterpret_cast<void**>(&only_once.p)) != S_OK ||
                !only_once.p) return false;
            out = reinterpret_cast<void*>(uintptr_t{0x1234});
            if (factory.p->OpenFile(
                    kNativeLiteral, kObservedXboxDStorageFileIid,
                    &out) != E_INVALIDARG || out) return false;
            only_once.p->Release();
            only_once.p = nullptr;
            // A guest string that crosses into a now-unmapped page must
            // be rejected before copying. An unsuccessful snapshot must
            // leave its previous host output untouched.
            std::memcpy(guest_path, nested_name.c_str(),
                        (nested_name.size() + 1) * sizeof(wchar_t));
            uint8_t* const second = owned_pages.alias + mem.page_bytes;
            if (mem.map_pages(second, 1, nullptr) != second)
                return false;
            copied = L"unchanged";
            out = reinterpret_cast<void*>(uintptr_t{0x1234});
            if (mem.snapshot_mapped_alias_utf16(
                    guest_path, 240, copied) || copied != L"unchanged" ||
                factory.p->OpenFile(
                    guest_path, kObservedXboxDStorageFileIid,
                    &out) != E_INVALIDARG || out ||
                mem.map_pages(second, 1, &owned_pages.frames[1]) != second)
                return false;

            // The terminator must occur within 240 UTF-16 units. These
            // 241 nonzero units are all mapped but must not be accepted.
            std::fill_n(guest_path, 241, L'x');
            out = reinterpret_cast<void*>(uintptr_t{0x1234});
            if (factory.p->OpenFile(
                    guest_path, kObservedXboxDStorageFileIid,
                    &out) != E_INVALIDARG || out)
                return false;
            // A short NUL-terminated string does not require any page
            // after its terminator, even if that page is now unmapped.
            guest_path[0] = L'a';
            guest_path[1] = 0;
            if (mem.map_pages(second, 1, nullptr) != second)
                return false;
            copied = L"before";
            if (!mem.snapshot_mapped_alias_utf16(
                    guest_path, 240, copied) || copied != L"a" ||
                mem.map_pages(second, 1, &owned_pages.frames[1]) != second)
                return false;
        }
        // All generated guest path regions and their pages were freed.
        // The fixture does not retain a source pointer after OpenFile.
        if (memory::PhysicalMemory::instance().allocated_page_count() !=
                old_guest_pages ||
            memory::PhysicalMemory::instance().pinned_alias_count() !=
                old_guest_pins) return false;
        // If Windows allows an unprivileged directory symlink, try
        // traversing it to a generated test file OUTSIDE the mount.
        // The opened file's canonical HANDLE path must reject escape.
        fs::create_directories(L"reports");
        const std::wstring outside_name =
            L"dstorage_escape_" +
            std::to_wstring(GetCurrentProcessId()) + L".bin";
        cleanup.outside_file = fs::path(L"reports") / outside_name;
        {
            std::ofstream stream(cleanup.outside_file, std::ios::binary);
            if (!stream || !stream.put('z')) return false;
        }
        cleanup.symbolic_directory = cleanup.dir / L"link";
        const fs::path external = fs::absolute(L"reports");
        if (CreateSymbolicLinkW(cleanup.symbolic_directory.c_str(),
                external.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY |
                SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            out = reinterpret_cast<void*>(uintptr_t{0x1234});
            const std::wstring escape = L"link\\" + outside_name;
            if (fixture_open(factory.p,
                    escape.c_str(), kObservedXboxDStorageFileIid,
                    &out) != HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
                out) return false;
        }

        // Exercise raw COM vtable slot +0x20, not just the C++ declaration.
        auto** vtable = *reinterpret_cast<void***>(factory.p);
        const auto open_slot = reinterpret_cast<OpenFileFn>(vtable[4]);
        FileOwner owned{};
        constexpr const wchar_t* kRawHostPath = L"generated.bin";
        if (!open_slot ||
            register_host_dstorage_fixture_path(
                kRawHostPath, L"generated.bin") != S_OK)
            return false;
        const HRESULT raw_open = open_slot(
            factory.p, kRawHostPath, kObservedXboxDStorageFileIid,
            reinterpret_cast<void**>(&owned.p));
        clear_host_dstorage_fixture_path();
        if (raw_open != S_OK ||
            !owned.p || live_host_dstorage_files() != 1 ||
            active_host_dstorage_file_leases() != 1)
            return false;
        if (factory.p->SetStagingBufferSize(16*kMiB) !=
                HRESULT_FROM_WIN32(ERROR_BUSY) ||
            factory.p->SetStagingBufferSize(32*kMiB) !=
                HRESULT_FROM_WIN32(ERROR_BUSY))
            return false;

        IUnknown* unknown = nullptr;
        if (owned.p->QueryInterface(IID_IUnknown,
                reinterpret_cast<void**>(&unknown)) != S_OK ||
            unknown != static_cast<IUnknown*>(owned.p)) return false;
        unknown->Release();
        out = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (owned.p->QueryInterface(invalid, &out) != E_NOINTERFACE ||
            out || owned.p->QueryInterface(IID_IUnknown, nullptr) != E_POINTER)
            return false;

        // Every GetHandle returns an independently owned read-only handle.
        HANDLE first = owned.p->GetHandle();
        HANDLE second = owned.p->GetHandle();
        if (first == INVALID_HANDLE_VALUE ||
            second == INVALID_HANDLE_VALUE || first == second) {
            if (first != INVALID_HANDLE_VALUE) CloseHandle(first);
            if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
            return false;
        }
        LARGE_INTEGER size{};
        std::vector<uint8_t> actual(expected.size());
        DWORD read = 0;
        const bool copied =
            GetFileSizeEx(first, &size) &&
            static_cast<uint64_t>(size.QuadPart) == expected.size() &&
            ReadFile(second, actual.data(),
                     static_cast<DWORD>(actual.size()), &read, nullptr) &&
            read == actual.size() &&
            std::equal(actual.begin(), actual.end(), expected.begin());
        CloseHandle(first);
        if (!copied) { CloseHandle(second); return false; }

        // Close releases the object's own I/O handle, not callers'
        // duplicated handles. The object still holds the staging lease
        // until its FINAL COM Release.
        owned.p->Close();
        owned.p->Close();
        if (owned.p->GetHandle() != INVALID_HANDLE_VALUE ||
            factory.p->SetStagingBufferSize(16*kMiB) !=
                HRESULT_FROM_WIN32(ERROR_BUSY)) {
            CloseHandle(second);
            return false;
        }
        LARGE_INTEGER borrowed_size{};
        if (!GetFileSizeEx(second, &borrowed_size) ||
            borrowed_size.QuadPart != size.QuadPart) {
            CloseHandle(second);
            return false;
        }
        CloseHandle(second);
        owned.p->Release();
        owned.p = nullptr;
        if (live_host_dstorage_files() ||
            active_host_dstorage_file_leases() ||
            factory.p->SetStagingBufferSize(16*kMiB) != S_OK)
            return false;

        // Independent concurrent COM file lifetimes and handle duplicates.
        std::atomic<bool> passed{true};
        std::vector<std::jthread> workers;
        for (unsigned t=0; t<4; ++t) {
            workers.emplace_back([&]{
                for (unsigned i=0; i<24; ++i) {
                    FileOwner local{};
                    if (fixture_open(factory.p, L"generated.bin",
                            kObservedXboxDStorageFileIid,
                            reinterpret_cast<void**>(&local.p)) != S_OK ||
                        !local.p) {
                        passed.store(false);
                        return;
                    }
                    HANDLE h = local.p->GetHandle();
                    if (h == INVALID_HANDLE_VALUE) {
                        passed.store(false);
                        return;
                    }
                    LARGE_INTEGER observed{};
                    if (!GetFileSizeEx(h, &observed) ||
                        observed.QuadPart !=
                            static_cast<LONGLONG>(expected.size()))
                        passed.store(false);
                    CloseHandle(h);
                }
            });
        }
        for (auto& worker : workers) worker.join();
        if (!passed.load() || live_host_dstorage_files() ||
            active_host_dstorage_file_leases())
            return false;
        clear_dstorage_file_fixture_root();
        out = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (fixture_open(factory.p, L"generated.bin",
                kObservedXboxDStorageFileIid, &out) != E_NOTIMPL ||
            out || factory.p->SetStagingBufferSize(32*kMiB) != S_OK)
            return false;
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::storage
