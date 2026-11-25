#include "apu_shape.h"
#include "apu_memory.h"
#include <mutex>

namespace scarlett::audio {
namespace {
class ShapeHost final {
public:
    static ShapeHost& instance() noexcept {
        static ShapeHost state;
        return state;
    }

    HRESULT allocate(ShapeContextRequest* request) noexcept {
        if (!request)
            return E_POINTER;
        const std::lock_guard lock(mutex_);
        // One active descriptor belongs to the process-wide HAL SHAPE
        // service until explicit release. Reject conflicting ownership.
        if (active_) {
            return request == active_ &&
                   request->counts[4] == count_ &&
                   request->arrays[4] == storage_
                       ? S_OK
                       : HRESULT_FROM_WIN32(ERROR_BUSY);
        }

        for (unsigned family = 0; family < 6; ++family) {
            if (family != 4 && request->counts[family] != 0)
                return E_NOTIMPL;
            if (request->arrays[family])
                return E_INVALIDARG;
        }
        const uint32_t count = request->counts[4];
        if (count == 0 || count > kHostMaxShapeContexts)
            return E_INVALIDARG;

        // Supported record stride is independently verified only for
        // the count in index 4. The bounded multiply fits uint32_t.
        const uint32_t bytes = count * kHostShapeRecordBytes;
        void* storage = nullptr;
        const HRESULT hr = ApuAlloc(&storage, nullptr, bytes, 2048, 0);
        if (FAILED(hr))
            return hr;
        if (!storage)
            return E_FAIL;

        // Complete ApuAlloc succeeds before touching any guest output
        // pointer. Thus failure cannot leak a partial SHAPE context set.
        request->arrays[4] = storage;
        active_ = request;
        storage_ = storage;
        count_ = count;
        return S_OK;
    }

    HRESULT release() noexcept {
        const std::lock_guard lock(mutex_);
        if (!active_)
            return S_OK;
        ShapeContextRequest* owner = active_;
        void* ptr = storage_;
        // The request is caller-owned and must outlive its HAL lifetime.
        if (owner->arrays[4] == ptr)
            owner->arrays[4] = nullptr;
        active_ = nullptr;
        storage_ = nullptr;
        count_ = 0;
        ApuFree(ptr);
        return S_OK;
    }

    ~ShapeHost() {
        // At exit the guest's descriptor may no longer be mapped.
        // The owned allocation registry releases surviving storage.
    }
private:
    std::mutex mutex_;
    ShapeContextRequest* active_ = nullptr;
    void* storage_ = nullptr;
    uint32_t count_ = 0;
};
} // namespace

extern "C" HRESULT WINAPI AcpHalAllocateShapeContexts(
    ShapeContextRequest* request) noexcept {
    return ShapeHost::instance().allocate(request);
}
extern "C" HRESULT WINAPI AcpHalReleaseShapeContexts() noexcept {
    return ShapeHost::instance().release();
}

FARPROC find_shape_export(std::string_view name) noexcept {
    if (name == "AcpHalAllocateShapeContexts")
        return reinterpret_cast<FARPROC>(&AcpHalAllocateShapeContexts);
    if (name == "AcpHalReleaseShapeContexts")
        return reinterpret_cast<FARPROC>(&AcpHalReleaseShapeContexts);
    return nullptr;
}
} // namespace scarlett::audio
