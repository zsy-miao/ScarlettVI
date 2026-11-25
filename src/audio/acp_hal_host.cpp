#include "acp_hal_host.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <new>

namespace scarlett::audio {
namespace {

constexpr uint32_t kMaxHostRingBytes = 1024u * 1024u;

struct HalState {
    HostHalObject guest{};
    std::mutex mutex;
    std::unique_ptr<uint8_t[]> input;
    std::unique_ptr<uint8_t[]> output;
    uint32_t input_bytes = 0;
    uint32_t output_bytes = 0;
};

std::atomic<size_t> g_live_objects{0};
std::atomic<size_t> g_live_buffers{0};
std::atomic<uint64_t> g_live_buffer_bytes{0};

HalState* host_state(HostHalObject* guest) noexcept {
    return reinterpret_cast<HalState*>(guest);
}

HRESULT WINAPI hal_start(HostHalObject* guest,
                         uint32_t input_bytes,
                         uint32_t output_bytes) noexcept {
    if (!guest)
        return E_POINTER;
    if (!input_bytes || !output_bytes ||
        input_bytes > kMaxHostRingBytes ||
        output_bytes > kMaxHostRingBytes)
        return E_INVALIDARG;
    auto& state = *host_state(guest);
    const std::lock_guard lock(state.mutex);
    if (state.input || state.output) {
        return state.input_bytes == input_bytes &&
               state.output_bytes == output_bytes
                   ? S_OK
                   : HRESULT_FROM_WIN32(ERROR_BUSY);
    }

    // Actual writable, zero-initialized Windows buffers, not a fabricated
    // successful guest-device initialization or Xbox DSP ring allocation.
    auto in = std::unique_ptr<uint8_t[]>(
        new (std::nothrow) uint8_t[input_bytes]{});
    if (!in)
        return E_OUTOFMEMORY;
    auto out = std::unique_ptr<uint8_t[]>(
        new (std::nothrow) uint8_t[output_bytes]{});
    if (!out)
        return E_OUTOFMEMORY;
    state.input = std::move(in);
    state.output = std::move(out);
    state.input_bytes = input_bytes;
    state.output_bytes = output_bytes;
    g_live_buffers.fetch_add(2, std::memory_order_relaxed);
    g_live_buffer_bytes.fetch_add(uint64_t{input_bytes} + output_bytes,
                                  std::memory_order_relaxed);
    return S_OK;
}

HRESULT WINAPI hal_stop(HostHalObject* guest) noexcept {
    if (!guest)
        return E_POINTER;
    auto& state = *host_state(guest);
    const std::lock_guard lock(state.mutex);
    if (state.input || state.output) {
        g_live_buffer_bytes.fetch_sub(uint64_t{state.input_bytes} +
                                      state.output_bytes,
                                      std::memory_order_relaxed);
        g_live_buffers.fetch_sub(2, std::memory_order_relaxed);
        state.input.reset();
        state.output.reset();
        state.input_bytes = 0;
        state.output_bytes = 0;
    }
    return S_OK;
}

HRESULT WINAPI hal_configure_unimplemented(HostHalObject* guest,
                                           uint32_t endpoint,
                                           uint32_t mask,
                                           uint32_t route,
                                           const uint32_t* value,
                                           uint32_t options) noexcept {
    // The title passes endpoint=2, mask=0xF0F0F0F0, route=0xFFFFFFFF,
    // value=&0xC0, options=0. No public contract establishes the
    // expected DSP operation. Do NOT claim success, dereference arbitrary
    // guest pointers, overwrite the value, or issue host audio commands.
    (void)guest;
    (void)endpoint;
    (void)mask;
    (void)route;
    (void)value;
    (void)options;
    return E_NOTIMPL;
}

HRESULT WINAPI hal_destroy(HostHalObject* guest) noexcept {
    if (!guest)
        return E_POINTER;
    const HRESULT stop_result = hal_stop(guest);
    if (FAILED(stop_result))
        return stop_result;
    delete host_state(guest);
    g_live_objects.fetch_sub(1, std::memory_order_relaxed);
    return S_OK;
}

constexpr HostHalVtable kHalVtable{
    &hal_start,
    &hal_stop,
    &hal_configure_unimplemented,
    nullptr, // slot +0x18 not observed
    nullptr, // slot +0x20 not observed
    &hal_destroy
};
} // namespace

extern "C" HRESULT WINAPI AcpHalCreate(HostHalObject** output) noexcept {
    if (!output)
        return E_POINTER;
    *output = nullptr;
    auto* state = new (std::nothrow) HalState;
    if (!state)
        return E_OUTOFMEMORY;
    state->guest.vtable = &kHalVtable;
    g_live_objects.fetch_add(1, std::memory_order_relaxed);
    *output = &state->guest;
    return S_OK;
}

FARPROC find_acphal_factory_export(std::string_view symbol) noexcept {
    return symbol == "AcpHalCreate"
        ? reinterpret_cast<FARPROC>(&AcpHalCreate)
        : nullptr;
}

HostHalStats host_hal_stats() noexcept {
    return {g_live_objects.load(std::memory_order_relaxed),
            g_live_buffers.load(std::memory_order_relaxed),
            g_live_buffer_bytes.load(std::memory_order_relaxed)};
}

bool inspect_host_hal_buffers_for_fixture(const HostHalObject* object,
                                          HostHalBuffers* out) noexcept {
    if (!object || !out || object->vtable != &kHalVtable)
        return false;
    auto* state = const_cast<HalState*>(
        reinterpret_cast<const HalState*>(object));
    const std::lock_guard lock(state->mutex);
    *out = {state->input.get(), state->output.get(),
            state->input_bytes, state->output_bytes};
    return true;
}
} // namespace scarlett::audio
