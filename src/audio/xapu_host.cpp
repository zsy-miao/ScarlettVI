#include "xapu_host.h"
#include <array>
#include <memory>
#include <mutex>
#include <vector>

namespace scarlett::audio {
struct HostXApuConnection {
    std::array<XApuResultEnvelope, kHostXApuResultCapacity> results{};
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;
    bool active = true;
};

namespace {
class XApuState {
public:
    static XApuState& instance() noexcept {
        static XApuState state;
        return state;
    }

    HRESULT connect(const void* descriptor, const void* callbacks,
                    HostXApuConnection** output) noexcept {
        if (!output)
            return E_POINTER;
        *output = nullptr;
        // These are opaque guest-owned descriptors. We know the title
        // passes two non-null pointers, but their fields are NOT verified.
        // Never dereference or retain them across their original lifetime.
        if (!descriptor || !callbacks)
            return E_INVALIDARG;

        try {
            std::lock_guard lock(mutex_);
            if (connections_.size() >= kHostXApuMaxConnections)
                return E_OUTOFMEMORY;
            std::unique_ptr<HostXApuConnection> connection =
                std::make_unique<HostXApuConnection>();
            HostXApuConnection* const address = connection.get();
            connections_.push_back(std::move(connection));
            *output = address;
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    HRESULT disconnect(HostXApuConnection* connection) noexcept {
        std::lock_guard lock(mutex_);
        HostXApuConnection* live = find_live(connection);
        if (!live)
            return E_INVALIDARG;
        live->active = false;
        live->count = 0;
        live->head = 0;
        live->tail = 0;
        // Tombstone handles are deliberately never reused; a stale guest
        // handle cannot become valid for a different connection.
        return S_OK;
    }

    uint32_t count(const HostXApuConnection* connection,
                   uint32_t selector) noexcept {
        if (selector != 0)
            return 0;
        std::lock_guard lock(mutex_);
        const auto* live = find_live(connection);
        return live ? static_cast<uint32_t>(live->count) : 0u;
    }

    HRESULT dequeue(HostXApuConnection* connection,
                    XApuResultEnvelope* output) noexcept {
        if (!output)
            return E_POINTER;
        std::lock_guard lock(mutex_);
        HostXApuConnection* live = find_live(connection);
        if (!live)
            return E_INVALIDARG;
        if (live->count == 0)
            return kHostXApuPendingResults;
        // This result was explicitly injected by a synthetic host fixture;
        // no unknown guest-command success is ever manufactured.
        *output = live->results[live->head];
        live->head = (live->head + 1) % kHostXApuResultCapacity;
        --live->count;
        return S_OK;
    }

    bool inject(HostXApuConnection* connection,
                const XApuResultEnvelope& result) noexcept {
        std::lock_guard lock(mutex_);
        HostXApuConnection* live = find_live(connection);
        if (!live || live->count == kHostXApuResultCapacity)
            return false;
        live->results[live->tail] = result;
        live->tail = (live->tail + 1) % kHostXApuResultCapacity;
        ++live->count;
        return true;
    }

    HostXApuStats stats() noexcept {
        std::lock_guard lock(mutex_);
        HostXApuStats report{connections_.size(), 0, 0};
        for (const auto& ptr : connections_)
            if (ptr->active) {
                ++report.active_connections;
                report.pending_results += ptr->count;
            }
        return report;
    }
private:
    HostXApuConnection* find_live(
        const HostXApuConnection* connection) noexcept {
        if (!connection)
            return nullptr;
        for (const auto& ptr : connections_) {
            if (ptr.get() == connection)
                return ptr->active ? ptr.get() : nullptr;
        }
        return nullptr;
    }
    std::mutex mutex_;
    std::vector<std::unique_ptr<HostXApuConnection>> connections_;
};
} // namespace

extern "C" HRESULT WINAPI XApuConnect(
    const void* descriptor, const void* callbacks,
    HostXApuConnection** output) noexcept {
    return XApuState::instance().connect(descriptor, callbacks, output);
}
extern "C" HRESULT WINAPI XApuDequeueResult(
    HostXApuConnection* connection, XApuResultEnvelope* output) noexcept {
    return XApuState::instance().dequeue(connection, output);
}
extern "C" HRESULT WINAPI XApuDisconnect(
    HostXApuConnection* connection) noexcept {
    return XApuState::instance().disconnect(connection);
}
extern "C" uint32_t WINAPI XApuGetResultCount(
    const HostXApuConnection* connection, uint32_t selector) noexcept {
    return XApuState::instance().count(connection, selector);
}

FARPROC find_xapu_export(std::string_view name) noexcept {
    if (name == "XApuConnect")
        return reinterpret_cast<FARPROC>(&XApuConnect);
    if (name == "XApuDequeueResult")
        return reinterpret_cast<FARPROC>(&XApuDequeueResult);
    if (name == "XApuDisconnect")
        return reinterpret_cast<FARPROC>(&XApuDisconnect);
    if (name == "XApuGetResultCount")
        return reinterpret_cast<FARPROC>(&XApuGetResultCount);
    // Real command processing remains unimplemented; never bind a
    // fake-success XApuEnqueueCommand stub.
    return nullptr;
}
HostXApuStats host_xapu_stats() noexcept {
    return XApuState::instance().stats();
}
bool inject_synthetic_xapu_result_for_fixture(
    HostXApuConnection* connection,
    const XApuResultEnvelope& result) noexcept {
    return XApuState::instance().inject(connection, result);
}
} // namespace scarlett::audio
