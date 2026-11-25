#include "dstorage_file_host.h"
#include <atomic>
#include <cwctype>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <utility>

namespace scarlett::storage {
namespace {
std::mutex root_mutex;
std::filesystem::path configured_root;
std::atomic<uint32_t> live_files{0};

HRESULT from_last_error() noexcept {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

// Single leaf only: reject directory escapes, ADS, Windows device names,
// trailing dot/space and all non-ASCII punctuation. This permits generated
// test data without inferring title-specific virtual filesystem semantics.
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
        stem[3] >= L'1' && stem[3] <= L'9')
        return false;
    return true;
}

class HostFile final : public HostDStorageFileX {
public:
    explicit HostFile(HANDLE handle) noexcept : handle_(handle) {
        live_files.fetch_add(1, std::memory_order_relaxed);
    }
    ~HostFile() {
        Close();
        release_host_dstorage_file_lease();
        live_files.fetch_sub(1, std::memory_order_relaxed);
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
};
} // namespace

HRESULT configure_dstorage_file_fixture_root(
    const std::filesystem::path& trusted_root) noexcept {
    try {
        const auto absolute = std::filesystem::absolute(trusted_root);
        const DWORD attributes = GetFileAttributesW(absolute.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
        std::lock_guard lock(root_mutex);
        if (!configured_root.empty())
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        configured_root = absolute;
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
    const wchar_t* leaf, REFIID iid, void** output) noexcept {
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
        if (!leaf) return E_INVALIDARG;
        const size_t length = wcsnlen_s(leaf, 97);
        if (!length || length > 96)
            return E_INVALIDARG;
        const std::wstring filename(leaf, length);
        if (!valid_leaf(filename)) return E_INVALIDARG;
        const auto absolute = root / filename;
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

uint32_t live_host_dstorage_files() noexcept {
    return live_files.load(std::memory_order_relaxed);
}
} // namespace scarlett::storage
