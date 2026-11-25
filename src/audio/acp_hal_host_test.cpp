#include "acp_hal_host.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

namespace scarlett::audio {
namespace {
using CreateFn = HRESULT (WINAPI*)(HostHalObject**) noexcept;
static_assert(std::is_same_v<decltype(&AcpHalCreate), CreateFn>);
using StartFn = HRESULT (WINAPI*)(HostHalObject*, uint32_t, uint32_t) noexcept;
using StopFn = HRESULT (WINAPI*)(HostHalObject*) noexcept;
using ConfigureFn = HRESULT (WINAPI*)(HostHalObject*, uint32_t,
    uint32_t, uint32_t, const uint32_t*, uint32_t) noexcept;
using DestroyFn = HRESULT (WINAPI*)(HostHalObject*) noexcept;
static_assert(std::is_same_v<
    decltype(HostHalVtable::start), StartFn>);
static_assert(std::is_same_v<
    decltype(HostHalVtable::stop), StopFn>);
static_assert(std::is_same_v<
    decltype(HostHalVtable::configure), ConfigureFn>);
static_assert(std::is_same_v<
    decltype(HostHalVtable::destroy), DestroyFn>);

bool test_individual(CreateFn create) {
    HostHalObject* first = nullptr;
    HostHalObject* second = nullptr;
    if (create(&first) != S_OK || !first || !first->vtable)
        return false;
    const auto* table = first->vtable;
    if (!table->start || !table->stop || !table->configure ||
        !table->destroy || table->unverified18 || table->unverified20) {
        table->destroy(first);
        return false;
    }
    if (create(&second) != S_OK || !second) {
        table->destroy(first);
        return false;
    }
    bool valid = true;
    valid &= table->start(first, 0u, 4096u) == E_INVALIDARG;
    valid &= table->start(first, 4096u, 0u) == E_INVALIDARG;
    valid &= table->start(first, 1024u * 1024u + 1u, 4096u)
             == E_INVALIDARG;
    valid &= table->start(first, 4096u, 4096u) == S_OK;
    valid &= table->start(first, 4096u, 4096u) == S_OK;
    valid &= table->start(first, 2048u, 4096u) ==
             HRESULT_FROM_WIN32(ERROR_BUSY);
    valid &= table->start(second, 1024u, 2048u) == S_OK;

    HostHalBuffers a, b;
    valid &= inspect_host_hal_buffers_for_fixture(first, &a);
    valid &= inspect_host_hal_buffers_for_fixture(second, &b);
    valid &= a.input && a.output && b.input && b.output &&
             a.input != a.output && a.input != b.input &&
             a.input_bytes == 4096u && a.output_bytes == 4096u &&
             b.input_bytes == 1024u && b.output_bytes == 2048u;

    if (a.input && a.output) {
        // Prove these are real writable and zero-initialized host buffers.
        auto* input = const_cast<uint8_t*>(a.input);
        auto* output = const_cast<uint8_t*>(a.output);
        for (uint32_t i = 0; i < a.input_bytes; ++i)
            valid &= input[i] == 0;
        for (uint32_t i = 0; i < a.output_bytes; ++i)
            valid &= output[i] == 0;
        input[0] = 0x5a;
        output[4095u] = 0xa5;
        valid &= input[0] == 0x5a && output[4095u] == 0xa5;
    }

    const auto active = host_hal_stats();
    valid &= active.live_objects >= 2 &&
             active.live_buffers >= 4 &&
             active.live_buffer_bytes >= (4096u * 2u + 1024u + 2048u);

    uint32_t config_value = 0xc0u;
    valid &= table->configure(first, 2u, 0xf0f0f0f0u,
                              0xffffffffu, &config_value, 0u)
             == E_NOTIMPL;
    valid &= config_value == 0xc0u;
    const auto unchanged = host_hal_stats();
    valid &= unchanged.live_objects == active.live_objects &&
             unchanged.live_buffers == active.live_buffers &&
             unchanged.live_buffer_bytes == active.live_buffer_bytes;

    valid &= table->stop(first) == S_OK;
    valid &= table->stop(first) == S_OK;
    valid &= inspect_host_hal_buffers_for_fixture(first, &a);
    valid &= a.input == nullptr && a.output == nullptr &&
             a.input_bytes == 0 && a.output_bytes == 0;
    valid &= table->start(first, 256u, 512u) == S_OK;
    valid &= table->destroy(first) == S_OK;
    valid &= table->destroy(second) == S_OK;
    return valid;
}
} // namespace

bool verify_host_hal_factory_fixture() noexcept {
    try {
        FARPROC address = find_acphal_factory_export("AcpHalCreate");
        if (address != reinterpret_cast<FARPROC>(&AcpHalCreate) ||
            find_acphal_factory_export("AcpHalCreateOrdinal") ||
            find_acphal_factory_export("acphalcreate") ||
            find_acphal_factory_export("AcpHalAllocateShapeContexts") ||
            find_acphal_factory_export(""))
            return false;
        const auto create = reinterpret_cast<CreateFn>(address);
        if (create(nullptr) != E_POINTER)
            return false;
        HostHalObject* sentinel =
            reinterpret_cast<HostHalObject*>(uintptr_t{0x1234});
        if (create(&sentinel) != S_OK || !sentinel)
            return false;
        const auto* vtable = sentinel->vtable;
        if (vtable->destroy(sentinel) != S_OK)
            return false;

        const auto before = host_hal_stats();
        if (!test_individual(create))
            return false;

        std::atomic<bool> pass{true};
        std::vector<std::jthread> threads;
        threads.reserve(4);
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (uint32_t i = 0; i < 128; ++i) {
                    HostHalObject* obj = nullptr;
                    if (create(&obj) != S_OK || !obj) {
                        pass.store(false);
                        return;
                    }
                    const auto* vt = obj->vtable;
                    const auto size = 256u + i + static_cast<uint32_t>(t);
                    if (vt->start(obj, size, size) != S_OK ||
                        vt->configure(obj, 2, 0xf0f0f0f0, 0xffffffffu,
                                      nullptr, 0) != E_NOTIMPL ||
                        vt->stop(obj) != S_OK) {
                        pass.store(false);
                    }
                    if (vt->destroy(obj) != S_OK)
                        pass.store(false);
                }
            });
        }
        for (auto& thread : threads)
            thread.join();
        const auto after = host_hal_stats();
        return pass.load() &&
               after.live_objects == before.live_objects &&
               after.live_buffers == before.live_buffers &&
               after.live_buffer_bytes == before.live_buffer_bytes;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::audio
