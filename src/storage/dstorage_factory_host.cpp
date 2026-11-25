#include "dstorage_factory_host.h"
#include <atomic>
#include <limits>
#include <mutex>
#include <vector>

namespace scarlett::storage {
namespace {
class HostFactory final : public HostDStorageFactoryX2 {
public:
    // The Xbox API returns a process-static singleton, not an owned
    // desktop DirectStorage factory. Keeping this object process-static
    // prevents dangling COM pointers across valid AddRef/Release cycles.
    static HostFactory& instance() noexcept {
        static HostFactory value;
        return value;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!IsEqualGUID(iid, kObservedXboxDStorageFactoryIid) &&
            !IsEqualGUID(iid, IID_IUnknown))
            return E_NOINTERFACE;
        std::lock_guard lock(mutex_);
        if (references_ == std::numeric_limits<ULONG>::max())
            return E_FAIL;
        ++references_;
        *output = static_cast<HostDStorageFactoryX2*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        std::lock_guard lock(mutex_);
        if (references_ == std::numeric_limits<ULONG>::max())
            return references_;
        return ++references_;
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        std::lock_guard lock(mutex_);
        if (!references_) return 0;
        return --references_;
    }
    HRESULT STDMETHODCALLTYPE CreateQueue(
        const void*, REFIID, void** output) noexcept override {
        // No Xbox I/O queue implementation, so do not return a fake
        // working queue or a PC DirectStorage COM pointer.
        if (!output) return E_POINTER;
        *output = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateStatusArray(
        uint32_t, const char*, REFIID, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE OpenFile(
        const wchar_t*, REFIID, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetCpuAffinity(uint64_t) noexcept override {
        // Never pretend that Xbox worker-affinity policy was installed.
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE SetDebugFlags(uint32_t) noexcept override {
        // Console-only debug flags aren't interpreted; no queue may
        // claim to provide validations controlled by these flags.
    }
    HRESULT STDMETHODCALLTYPE SetStagingBufferSize(
        uint32_t bytes) noexcept override {
        // Public GDK documents exactly 0,16,20,24,28,32MiB.
        // Note that heap quotas here are a HOST policy; not console RAM.
        constexpr uint32_t mib = 1024u * 1024u;
        if (bytes != 0 && bytes != 16*mib && bytes != 20*mib &&
            bytes != 24*mib && bytes != 28*mib && bytes != 32*mib)
            return E_INVALIDARG;
        std::lock_guard lock(mutex_);
        // Public contract forbids staging resize while any queue or file
        // exists, including attempts to set the same buffer size.
        if (active_host_queues_ != 0)
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        if (bytes == staging_.size())
            return S_OK;
        try {
            std::vector<uint8_t> candidate(bytes, uint8_t{0});
            staging_.swap(candidate);
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }
    void STDMETHODCALLTYPE GetStats(void*) noexcept override {
        // The Xbox statistics output structure is not verified and
        // this method has void return: NEVER write an unknown-sized
        // caller buffer pretending to supply hardware stats.
    }
    HRESULT STDMETHODCALLTYPE CreateQueue1(
        const void*, REFIID, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return E_NOTIMPL;
    }

    HRESULT initialize_default() noexcept {
        // This is a real host-owned 32MiB staging buffer allocated on
        // first factory lookup, matching public DirectStorage behavior.
        std::lock_guard lock(mutex_);
        if (initialized_) return S_OK;
        try {
            staging_.assign(kDefaultStagingBytes, uint8_t{0});
            initialized_ = true;
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }
    bool acquire_host_queue_lease() noexcept {
        const std::lock_guard lock(mutex_);
        if (!initialized_ || staging_.empty() ||
            active_host_queues_ == std::numeric_limits<uint32_t>::max())
            return false;
        ++active_host_queues_;
        return true;
    }
    void release_host_queue_lease() noexcept {
        const std::lock_guard lock(mutex_);
        if (active_host_queues_) --active_host_queues_;
    }
    uint32_t active_host_queue_leases() noexcept {
        const std::lock_guard lock(mutex_);
        return active_host_queues_;
    }
    uint32_t staging_size() noexcept {
        std::lock_guard lock(mutex_);
        return static_cast<uint32_t>(staging_.size());
    }
    bool probe_pages() noexcept {
        std::lock_guard lock(mutex_);
        if (staging_.empty()) return false;
        // Touch actual committed host pages and verify round-trip data.
        // Fixture restores original contents; not a guest service.
        constexpr size_t stride = 64u * 1024u;
        for (size_t pos=0; pos<staging_.size(); pos+=stride) {
            const uint8_t original = staging_[pos];
            const uint8_t marker = static_cast<uint8_t>(
                (pos / stride * 41u + 0x5bu) & 255u);
            staging_[pos] = marker;
            if (staging_[pos] != marker) {
                staging_[pos] = original;
                return false;
            }
            staging_[pos] = original;
        }
        return true;
    }
private:
    std::mutex mutex_;
    std::vector<uint8_t> staging_;
    ULONG references_ = 0;
    bool initialized_ = false;
    uint32_t active_host_queues_ = 0;
};
}

extern "C" HRESULT WINAPI DStorageGetFactory(
    REFIID iid, void** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!IsEqualGUID(iid, kObservedXboxDStorageFactoryIid))
        return E_NOINTERFACE;
    auto& factory = HostFactory::instance();
    const HRESULT hr = factory.initialize_default();
    if (FAILED(hr)) return hr;
    return factory.QueryInterface(iid, output);
}

FARPROC find_dstorage_export(std::string_view name) noexcept {
    return name == "DStorageGetFactory"
        ? reinterpret_cast<FARPROC>(&DStorageGetFactory) : nullptr;
}
bool acquire_host_dstorage_queue_lease() noexcept {
    return HostFactory::instance().acquire_host_queue_lease();
}
void release_host_dstorage_queue_lease() noexcept {
    HostFactory::instance().release_host_queue_lease();
}
uint32_t active_host_dstorage_queue_leases() noexcept {
    return HostFactory::instance().active_host_queue_leases();
}
uint32_t host_dstorage_staging_bytes() noexcept {
    return HostFactory::instance().staging_size();
}
bool verify_dstorage_staging_pages_for_fixture() noexcept {
    return HostFactory::instance().probe_pages();
}
} // namespace scarlett::storage
