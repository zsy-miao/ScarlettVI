#include "dstorage_file_host.h"
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
    bool created = false;
    ~FixtureRoot() {
        clear_dstorage_file_fixture_root();
        if (created) {
            std::error_code error;
            fs::remove(file, error);
            fs::remove(dir, error);
        }
    }
};
using OpenFileFn = HRESULT (STDMETHODCALLTYPE*)(
    HostDStorageFactoryX2*, const wchar_t*, REFIID, void**) noexcept;
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
        if (factory.p->OpenFile(L"generated.bin",
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
        if (configure_dstorage_file_fixture_root(cleanup.dir) != S_OK ||
            configure_dstorage_file_fixture_root(cleanup.dir) !=
                HRESULT_FROM_WIN32(ERROR_BUSY)) return false;
        if (factory.p->OpenFile(L"generated.bin",
                kObservedXboxDStorageFileIid, nullptr) != E_POINTER)
            return false;

        const GUID invalid{0x4e934ab6,0x67ac,0x1234,{1,2,3,4,5,6,7,8}};
        out = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (factory.p->OpenFile(L"generated.bin", invalid, &out) !=
                E_NOINTERFACE || out) return false;
        constexpr std::array<const wchar_t*, 10> bad{
            L"", L".", L"..", L"..\\generated.bin", L"../generated.bin",
            L"C:generated.bin", L"CON.bin", L"foo.", L"foo bar.bin",
            L"generated.bin:secret"};
        for (const wchar_t* path : bad) {
            out = reinterpret_cast<void*>(uintptr_t{0x1234});
            if (factory.p->OpenFile(path, kObservedXboxDStorageFileIid,
                    &out) != E_INVALIDARG || out) return false;
        }
        out = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (factory.p->OpenFile(L"absent.bin",
                kObservedXboxDStorageFileIid, &out) !=
                HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || out) return false;

        // Exercise raw COM vtable slot +0x20, not just the C++ declaration.
        auto** vtable = *reinterpret_cast<void***>(factory.p);
        const auto open_slot = reinterpret_cast<OpenFileFn>(vtable[4]);
        FileOwner owned{};
        if (!open_slot ||
            open_slot(factory.p, L"generated.bin",
                      kObservedXboxDStorageFileIid,
                      reinterpret_cast<void**>(&owned.p)) != S_OK ||
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
                    if (factory.p->OpenFile(L"generated.bin",
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
        if (factory.p->OpenFile(L"generated.bin",
                kObservedXboxDStorageFileIid, &out) != E_NOTIMPL ||
            out || factory.p->SetStagingBufferSize(32*kMiB) != S_OK)
            return false;
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::storage
