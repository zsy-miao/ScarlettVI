#include "dstorage_status_host.h"
#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_set>
#include <vector>

namespace scarlett::storage {
namespace {
std::atomic<uint32_t> live_status_arrays{0};
std::mutex live_registry_mutex;
std::unordered_set<HostDStorageStatusArrayX*> live_registry;

class HostStatusArray final : public HostDStorageStatusArrayX {
public:
    explicit HostStatusArray(uint32_t capacity)
        : statuses_(capacity) {
        // Register only after the owned slot allocation succeeds.
        const std::lock_guard lock(live_registry_mutex);
        live_registry.insert(this);
        live_status_arrays.fetch_add(1, std::memory_order_relaxed);
    }
    ~HostStatusArray() {
        const std::lock_guard lock(live_registry_mutex);
        live_registry.erase(this);
        live_status_arrays.fetch_sub(1, std::memory_order_relaxed);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!IsEqualGUID(iid, IID_IUnknown) &&
            !IsEqualGUID(iid, kObservedXboxDStorageStatusIid))
            return E_NOINTERFACE;
        AddRef();
        *output = static_cast<HostDStorageStatusArrayX*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        // The host queue also owns a reference to every held status
        // marker, preventing destruction during active I/O.
        ULONG refs = references_.load(std::memory_order_relaxed);
        while (refs && refs != std::numeric_limits<ULONG>::max()) {
            if (references_.compare_exchange_weak(
                    refs, refs + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed))
                return refs + 1;
        }
        return refs;
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG refs = references_.load(std::memory_order_relaxed);
        while (refs) {
            if (references_.compare_exchange_weak(
                    refs, refs - 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                if (refs == 1) delete this;
                return refs - 1;
            }
        }
        return 0;
    }
    HRESULT STDMETHODCALLTYPE GetHResult(uint32_t index)
        noexcept override {
        const std::lock_guard lock(mutex_);
        if (index >= statuses_.size()) return E_INVALIDARG;
        return statuses_[index].status;
    }
    bool STDMETHODCALLTYPE IsComplete(uint32_t index)
        noexcept override {
        return GetHResult(index) != E_PENDING &&
               index < statuses_.size();
    }
    HRESULT begin(uint32_t index) noexcept {
        const std::lock_guard lock(mutex_);
        if (index >= statuses_.size()) return E_INVALIDARG;
        if (statuses_[index].in_flight)
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        statuses_[index].in_flight = true;
        statuses_[index].status = E_PENDING;
        return S_OK;
    }
    HRESULT finish(uint32_t index, HRESULT result) noexcept {
        const std::lock_guard lock(mutex_);
        if (index >= statuses_.size() || result == E_PENDING)
            return E_INVALIDARG;
        if (!statuses_[index].in_flight)
            return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        statuses_[index].in_flight = false;
        statuses_[index].status = result;
        return S_OK;
    }
private:
    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    struct Slot {
        HRESULT status = E_PENDING; // Undefined pre-enqueue: fail closed.
        bool in_flight = false;
    };
    std::vector<Slot> statuses_;
};
}

HRESULT create_host_dstorage_status_array(
    uint32_t capacity, const char*, REFIID iid,
    void** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!IsEqualGUID(iid, kObservedXboxDStorageStatusIid))
        return E_NOINTERFACE;
    // Conservative host quota, NOT the Xbox GDK capacity limit.
    if (!capacity || capacity > 256) return E_INVALIDARG;
    try {
        auto* object = new HostStatusArray(capacity);
        *output = static_cast<HostDStorageStatusArrayX*>(object);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}
HRESULT begin_host_dstorage_status(
    HostDStorageStatusArrayX* object, uint32_t index) noexcept {
    if (!object) return E_POINTER;
    // Internal-only function: the producer passes objects it received
    // from our factory, never arbitrary console memory.
    return static_cast<HostStatusArray*>(object)->begin(index);
}
HRESULT finish_host_dstorage_status(
    HostDStorageStatusArrayX* object, uint32_t index,
    HRESULT result) noexcept {
    if (!object) return E_POINTER;
    return static_cast<HostStatusArray*>(object)->finish(index, result);
}
bool retain_owned_dstorage_status_array(
    HostDStorageStatusArrayX* candidate) noexcept {
    if (!candidate) return false;
    // Match by address before DEREFERENCING: arbitrary guest pointers or
    // external COM objects are not safe for internal HostStatusArray casts.
    const std::lock_guard lock(live_registry_mutex);
    if (!live_registry.contains(candidate)) return false;
    // The registry lock also excludes the destructor while AddRef runs.
    return candidate->AddRef() != 0;
}
uint32_t live_host_dstorage_status_arrays() noexcept {
    return live_status_arrays.load(std::memory_order_relaxed);
}
} // namespace scarlett::storage
