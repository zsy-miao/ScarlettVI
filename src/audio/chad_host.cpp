#include "chad_host.h"
#include "dsp_convolution.h"
#include "../runtime/physical_memory.h"
#include "../storage/dstorage_alias_destination.h"
#include <array>
#include <memory>
#include <mutex>
#include <vector>

namespace scarlett::audio {

struct HostXDspClient {
    HostDspConvolver fir{};
    bool active = true;
    uint64_t completed = 0;
};

namespace {
class XDspRegistry final {
public:
    static XDspRegistry& instance() noexcept {
        static XDspRegistry value;
        return value;
    }

    HRESULT connect(const void* config, uint32_t stream_hint,
                    uint32_t stream_limit, uint32_t impulse_limit,
                    uint32_t flags, HostXDspClient** output) noexcept {
        if (!output) return E_POINTER;
        *output = nullptr;
        // The first argument is an opaque guest-owned handle; RDX
        // carries a 32-bit value at the observed callsite, NOT a
        // descriptor pointer. Do not read either argument. Limit values
        // are HOST policies, not claims about Xbox hardware caps.
        (void)stream_hint;
        if (!config || !stream_limit ||
            stream_limit > 128 || !impulse_limit ||
            impulse_limit > HostDspConvolver::kMaxTaps || flags != 1)
            return E_INVALIDARG;
        try {
            std::lock_guard lock(mutex_);
            // Tombstones are kept until shutdown, so an old handle
            // cannot silently become a new and different client.
            if (clients_.size() >= kMaxHostXDspClients)
                return static_cast<HRESULT>(0x8AC810A5u);
            auto object = std::make_unique<HostXDspClient>();
            auto* const address = object.get();
            clients_.push_back(std::move(object));
            *output = address;
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    HRESULT disconnect(HostXDspClient* client) noexcept {
        const std::lock_guard lock(mutex_);
        auto* const live = find_live(client);
        if (!live) return E_INVALIDARG;
        live->active = false;
        return S_OK;
    }

    HRESULT configure(HostXDspClient* client,
                      std::span<const float> taps,
                      uint32_t channels) noexcept {
        const std::lock_guard lock(mutex_);
        auto* const live = find_live(client);
        return live ? live->fir.configure(taps, channels) : E_INVALIDARG;
    }

    HRESULT submit(HostXDspClient* client, const void* command) noexcept {
        if (!command) return E_INVALIDARG;
        const std::lock_guard lock(mutex_);
        auto* const live = find_live(client);
        if (!live) return E_INVALIDARG;

        auto& memory = memory::PhysicalMemory::instance();
        HostXDspFirCommandV1 snapshot{};
        if (!memory.snapshot_mapped_alias(
                command, sizeof(snapshot), &snapshot))
            return E_NOTIMPL;
        if (snapshot.magic != kHostXDspFirCommandMagic ||
            snapshot.version != 1 || snapshot.reserved ||
            snapshot.reserved2 || snapshot.reserved3)
            return E_NOTIMPL;

        constexpr uint32_t kMaxSamples = 16 * 1024;
        if (!snapshot.input || !snapshot.output ||
            !snapshot.samples || snapshot.samples > kMaxSamples ||
            (reinterpret_cast<uintptr_t>(snapshot.input) & 3) ||
            (reinterpret_cast<uintptr_t>(snapshot.output) & 3))
            return E_INVALIDARG;

        const size_t bytes = static_cast<size_t>(snapshot.samples)
                             * sizeof(float);
        try {
            std::vector<float> samples(snapshot.samples);
            if (!memory.snapshot_mapped_alias(
                    snapshot.input, bytes, samples.data()))
                return E_NOTIMPL;
            memory::PhysicalMemory::MappedAliasSpan span{};
            if (!memory.resolve_mapped_alias_span(
                    snapshot.output, bytes, span))
                return E_NOTIMPL;

            std::shared_ptr<storage::HostDStorageAliasDestination> owned;
            const HRESULT pin = storage::HostDStorageAliasDestination::pin_pages(
                snapshot.output, span.frames.data(), span.count, bytes,
                owned);
            if (FAILED(pin)) return pin;

            // A real host-typed, synchronous FIR operation. The console's
            // unknown Xbox DSP commands are never treated as completed.
            // The pin survives the processing call and is automatically
            // released on all success/failure branches.
            const HRESULT hr = live->fir.process(
                samples,
                std::span<float>(snapshot.output, snapshot.samples));
            if (SUCCEEDED(hr)) ++live->completed;
            return hr;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    HostXDspStats stats() noexcept {
        std::lock_guard lock(mutex_);
        HostXDspStats result{clients_.size(), 0, 0};
        for (const auto& ptr : clients_) {
            if (ptr->active) ++result.live_clients;
            result.host_fir_commands += ptr->completed;
        }
        return result;
    }
private:
    HostXDspClient* find_live(const HostXDspClient* address) noexcept {
        if (!address) return nullptr;
        for (auto& ptr : clients_)
            if (ptr.get() == address)
                return ptr->active ? ptr.get() : nullptr;
        return nullptr;
    }
    std::mutex mutex_;
    std::vector<std::unique_ptr<HostXDspClient>> clients_;
};
} // namespace

extern "C" HRESULT WINAPI XDspConnectWithMaximumStreamLimit(
    const void* config, uint32_t stream_hint,
    uint32_t streams, uint32_t impulse, uint32_t flags,
    HostXDspClient** output) noexcept {
    return XDspRegistry::instance().connect(
        config, stream_hint, streams, impulse, flags, output);
}
extern "C" HRESULT WINAPI XDspDisconnect(
    HostXDspClient* client) noexcept {
    return XDspRegistry::instance().disconnect(client);
}

extern "C" HRESULT WINAPI XDspActivate(
    void* /*opaque_a*/, void* /*opaque_b*/, uintptr_t /*opaque_c*/,
    uintptr_t /*opaque_d*/, uintptr_t /*opaque_e*/) noexcept {
    // The Xbox XDspStream activation parameter ownership, hardware
    // impulse response routing and output type are not verified. Never
    // fabricate an active stream or write a guessed guest output.
    return E_NOTIMPL;
}
extern "C" HRESULT WINAPI XDspDeactivate(
    void* /*stream*/) noexcept {
    // No real Xbox stream was ever activated by this bridge.
    return E_NOTIMPL;
}
extern "C" HRESULT WINAPI XDspSubmitCommand(
    HostXDspClient* client, const void* command) noexcept {
    return XDspRegistry::instance().submit(client, command);
}

HRESULT configure_host_xdsp_fir(
    HostXDspClient* client, std::span<const float> taps,
    uint32_t channels) noexcept {
    return XDspRegistry::instance().configure(client, taps, channels);
}
HostXDspStats host_xdsp_stats() noexcept {
    return XDspRegistry::instance().stats();
}
FARPROC find_chad_export(std::string_view symbol) noexcept {
    if (symbol == "XDspConnectWithMaximumStreamLimit")
        return reinterpret_cast<FARPROC>(&XDspConnectWithMaximumStreamLimit);
    if (symbol == "XDspDisconnect")
        return reinterpret_cast<FARPROC>(&XDspDisconnect);
    if (symbol == "XDspActivate")
        return reinterpret_cast<FARPROC>(&XDspActivate);
    if (symbol == "XDspDeactivate")
        return reinterpret_cast<FARPROC>(&XDspDeactivate);
    if (symbol == "XDspSubmitCommand")
        return reinterpret_cast<FARPROC>(&XDspSubmitCommand);
    return nullptr;
}
} // namespace scarlett::audio
