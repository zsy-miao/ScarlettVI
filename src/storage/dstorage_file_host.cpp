#include "dstorage_file_host.h"
#include "../runtime/physical_memory.h"
#include <atomic>
#include <cwctype>
#include <vector>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <utility>

namespace scarlett::storage {
namespace {
std::mutex root_mutex;
std::filesystem::path configured_root;
struct HostFixturePathPermit {
    const wchar_t* pointer_identity = nullptr;
    std::wstring snapshot;
};
thread_local HostFixturePathPermit pending_host_path;
std::atomic<uint32_t> live_files{0};
class HostFile;
std::mutex file_registry_mutex;
HostFile* file_registry_head = nullptr;

HRESULT from_last_error() noexcept {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

// Each component rejects directory escapes, ADS, Windows device names,
// trailing dot/space and all non-ASCII punctuation. Relative paths here
// are generated-test paths, not verified Xbox VFS syntax.
bool valid_leaf(const std::wstring& leaf) {
    if (leaf.empty() || leaf.size() > 96 || leaf == L"." ||
        leaf == L".." || leaf.back() == L'.')
        return false;
    for (wchar_t c : leaf) {
        if (!((c >= L'A' && c <= L'Z') ||
              (c >= L'a' && c <= L'z') ||
              (c >= L'0' && c <= L'9') ||
              c == L'-' || c == L'_' || c == L'.'))
            return false;
    }
    const auto dot = leaf.find(L'.');
    auto stem = leaf.substr(0, dot);
    for (auto& c : stem) c = std::towupper(c);
    if (stem == L"CON" || stem == L"PRN" ||
        stem == L"NUL" || stem == L"AUX")
        return false;
    if (stem.size() == 4 &&
        (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
        stem[3] >= L'0' && stem[3] <= L'9')
        return false;
    return true;
}

// Convert a bounded generated guest relative path into individually
// validated Windows components. This is NOT verified Xbox VFS syntax.
bool parse_fixture_relative_path(
    const std::wstring& raw, std::filesystem::path& relative) {
    relative.clear();
    if (raw.empty() || raw.size() > 240) return false;
    size_t start = 0;
    unsigned depth = 0;
    for (size_t i = 0; i <= raw.size(); ++i) {
        if (i != raw.size() && raw[i] != L'/' && raw[i] != L'\\')
            continue;
        if (i == start || ++depth > 8) return false;
        const std::wstring segment = raw.substr(start, i - start);
        if (!valid_leaf(segment)) return false;
        relative /= segment;
        start = i + 1;
    }
    return true;
}

// Final HANDLE identity, not lexical pre-open paths, protects against
// junctions and symlinks replacing an intermediate directory.
bool final_path_of_handle(HANDLE handle, std::wstring& result) {
    result.clear();
    const DWORD needed = GetFinalPathNameByHandleW(
        handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!needed || needed > 32767) return false;
    std::vector<wchar_t> buffer(static_cast<size_t>(needed) + 1);
    const DWORD written = GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!written || written >= buffer.size()) return false;
    result.assign(buffer.data(), written);
    return true;
}
bool final_path_under_root(const std::wstring& root,
                           const std::wstring& file) noexcept {
    if (root.empty() || file.size() <= root.size() ||
        file.compare(0, root.size(), root) != 0)
        return false;
    return root.back() == L'\\' || file[root.size()] == L'\\';
}

class HostFile final : public HostDStorageFileX {
public:
    explicit HostFile(HANDLE handle) noexcept : handle_(handle) {
        const std::lock_guard lock(file_registry_mutex);
        next_registered_ = file_registry_head;
        file_registry_head = this;
        live_files.fetch_add(1, std::memory_order_relaxed);
    }
    ~HostFile() {
        {
            const std::lock_guard lock(file_registry_mutex);
            auto** link = &file_registry_head;
            while (*link && *link != this)
                link = &(*link)->next_registered_;
            if (*link == this) *link = next_registered_;
        }
        Close();
        release_host_dstorage_file_lease();
        live_files.fetch_sub(1, std::memory_order_relaxed);
    }
    HostFile* next_registered() const noexcept { return next_registered_; }
    HRESULT reopen_for_read(HANDLE* output) noexcept {
        if (!output) return E_POINTER;
        *output = INVALID_HANDLE_VALUE;
        const std::lock_guard lock(mutex_);
        if (handle_ == INVALID_HANDLE_VALUE)
            return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        // Unlike DuplicateHandle, ReOpenFile creates its own file pointer;
        // concurrent source reads cannot move one another's offsets.
        HANDLE handle = ReOpenFile(handle_, GENERIC_READ, FILE_SHARE_READ, 0);
        if (handle == INVALID_HANDLE_VALUE) return from_last_error();
        *output = handle;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!IsEqualGUID(iid, IID_IUnknown) &&
            !IsEqualGUID(iid, kObservedXboxDStorageFileIid))
            return E_NOINTERFACE;
        if (!AddRef()) return E_FAIL;
        *output = static_cast<HostDStorageFileX*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        ULONG refs = refs_.load(std::memory_order_relaxed);
        while (refs && refs != std::numeric_limits<ULONG>::max()) {
            if (refs_.compare_exchange_weak(refs, refs + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
                return refs + 1;
        }
        return 0;
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG refs = refs_.load(std::memory_order_relaxed);
        while (refs) {
            if (refs_.compare_exchange_weak(refs, refs - 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                if (refs == 1) delete this;
                return refs - 1;
            }
        }
        return 0;
    }
    void STDMETHODCALLTYPE Close() noexcept override {
        const std::lock_guard lock(mutex_);
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }
    HANDLE STDMETHODCALLTYPE GetHandle() noexcept override {
        const std::lock_guard lock(mutex_);
        if (handle_ == INVALID_HANDLE_VALUE)
            return INVALID_HANDLE_VALUE;
        HANDLE duplicate = INVALID_HANDLE_VALUE;
        if (!DuplicateHandle(GetCurrentProcess(), handle_,
                             GetCurrentProcess(), &duplicate,
                             0, FALSE, DUPLICATE_SAME_ACCESS))
            return INVALID_HANDLE_VALUE;
        return duplicate;
    }
private:
    std::atomic<ULONG> refs_{1};
    std::mutex mutex_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HostFile* next_registered_ = nullptr;
    friend HRESULT reopen_owned_dstorage_file_for_read(
        HostDStorageFileX*, HANDLE*) noexcept;
};
} // namespace

HRESULT register_host_dstorage_fixture_path(
    const wchar_t* pointer_identity,
    std::wstring trusted_snapshot) noexcept {
    if (!pointer_identity) return E_INVALIDARG;
    if (pending_host_path.pointer_identity)
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    try {
        pending_host_path.snapshot = std::move(trusted_snapshot);
        pending_host_path.pointer_identity = pointer_identity;
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

void clear_host_dstorage_fixture_path() noexcept {
    pending_host_path.pointer_identity = nullptr;
    pending_host_path.snapshot.clear();
}

HRESULT configure_dstorage_file_fixture_root(
    const std::filesystem::path& trusted_root) noexcept {
    try {
        const auto absolute = std::filesystem::absolute(trusted_root);
        HANDLE directory = CreateFileW(
            absolute.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (directory == INVALID_HANDLE_VALUE)
            return from_last_error();
        FILE_ATTRIBUTE_TAG_INFO info{};
        std::wstring resolved;
        const bool valid = GetFileInformationByHandleEx(
                directory, FileAttributeTagInfo, &info, sizeof(info)) &&
            (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            final_path_of_handle(directory, resolved);
        CloseHandle(directory);
        if (!valid)
            return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
        std::lock_guard lock(root_mutex);
        if (!configured_root.empty())
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        configured_root = std::filesystem::path(resolved);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

void clear_dstorage_file_fixture_root() noexcept {
    const std::lock_guard lock(root_mutex);
    configured_root.clear();
}
HRESULT snapshot_dstorage_file_fixture_root(
    std::filesystem::path& output) noexcept {
    try {
        const std::lock_guard lock(root_mutex);
        output.clear();
        if (configured_root.empty()) return E_NOTIMPL;
        output = configured_root;
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HRESULT open_host_dstorage_file(
    const wchar_t* relative_path, REFIID iid, void** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    try {
        std::filesystem::path root;
        {
            const std::lock_guard lock(root_mutex);
            if (configured_root.empty()) return E_NOTIMPL;
            root = configured_root;
        }
        if (!IsEqualGUID(iid, kObservedXboxDStorageFileIid))
            return E_NOINTERFACE;
        if (!relative_path) return E_INVALIDARG;
        std::wstring snapshot;
        if (pending_host_path.pointer_identity == relative_path) {
            // Host-generated tests may supply native literals only after
            // explicitly registering an immutable, thread-local one-shot
            // snapshot. Never dereference the original pointer.
            snapshot = std::move(pending_host_path.snapshot);
            clear_host_dstorage_fixture_path();
        } else if (!memory::PhysicalMemory::instance()
                       .snapshot_mapped_alias_utf16(
                           relative_path, 240, snapshot)) {
            // Unknown guest and host pointers are never dereferenced.
            return E_INVALIDARG;
        }
        if (snapshot.empty() || snapshot.size() > 240)
            return E_INVALIDARG;
        std::filesystem::path relative;
        if (!parse_fixture_relative_path(snapshot, relative))
            return E_INVALIDARG;
        const auto absolute = root / relative;
        HANDLE handle = CreateFileW(
            absolute.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return from_last_error();
        FILE_ATTRIBUTE_TAG_INFO info{};
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                           &info, sizeof(info))) {
            const HRESULT failure = from_last_error();
            CloseHandle(handle);
            return failure;
        }
        if (info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                   FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(handle);
            return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        }
        std::wstring resolved;
        if (!final_path_of_handle(handle, resolved)) {
            const HRESULT failure = from_last_error();
            CloseHandle(handle);
            return failure;
        }
        // Validate the final opened object after all symlink/junction
        // resolution. An intermediate reparse point leading outside
        // our trusted fixture directory must never escape the mount.
        if (!final_path_under_root(root.wstring(), resolved)) {
            CloseHandle(handle);
            return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        }
        if (!acquire_host_dstorage_file_lease()) {
            CloseHandle(handle);
            return HRESULT_FROM_WIN32(ERROR_NOT_READY);
        }
        auto* file = new (std::nothrow) HostFile(handle);
        if (!file) {
            release_host_dstorage_file_lease();
            CloseHandle(handle);
            return E_OUTOFMEMORY;
        }
        *output = static_cast<HostDStorageFileX*>(file);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HRESULT reopen_owned_dstorage_file_for_read(
    HostDStorageFileX* candidate, HANDLE* output) noexcept {
    if (!output) return E_POINTER;
    *output = INVALID_HANDLE_VALUE;
    if (!candidate) return E_POINTER;
    // Compare address first; no dereference of arbitrary guest pointers.
    // AddRef under the registry lock pins the object across reopening.
    HostFile* found = nullptr;
    {
        const std::lock_guard lock(file_registry_mutex);
        for (HostFile* current = file_registry_head; current;
             current = current->next_registered()) {
            if (static_cast<HostDStorageFileX*>(current) == candidate) {
                if (current->AddRef() != 0) found = current;
                break;
            }
        }
    }
    if (!found) return E_INVALIDARG;
    const HRESULT hr = found->reopen_for_read(output);
    found->Release();
    return hr;
}
uint32_t live_host_dstorage_files() noexcept {
    return live_files.load(std::memory_order_relaxed);
}
} // namespace scarlett::storage
