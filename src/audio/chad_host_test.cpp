#include "chad_host.h"
#include "../runtime/physical_memory.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace scarlett::audio {
namespace {
using Connect = HRESULT (WINAPI*)(
    const void*, uint32_t, uint32_t, uint32_t, uint32_t,
    HostXDspClient**) noexcept;
using Disconnect = HRESULT (WINAPI*)(HostXDspClient*) noexcept;
using Submit = HRESULT (WINAPI*)(HostXDspClient*, const void*) noexcept;
static_assert(std::is_same_v<decltype(&XDspConnectWithMaximumStreamLimit), Connect>);
static_assert(std::is_same_v<decltype(&XDspDisconnect), Disconnect>);
static_assert(std::is_same_v<decltype(&XDspSubmitCommand), Submit>);

struct MappedXMemFixture {
    static constexpr size_t pages = 5;
    std::array<uintptr_t, pages> frames{};
    uint8_t* base = nullptr;
    bool allocated = false;

    bool create() noexcept {
        auto& memory = memory::PhysicalMemory::instance();
        uintptr_t wanted = pages;
        if (!memory.allocate_pages(MEM_64K_PAGES, &wanted, frames.data()) ||
            wanted != pages)
            return false;
        allocated = true;
        base = static_cast<uint8_t*>(memory.reserve_alias_region(
            nullptr, pages * memory.page_bytes));
        return base && memory.map_pages(
            base, pages, frames.data()) == base;
    }
    ~MappedXMemFixture() noexcept {
        auto& memory = memory::PhysicalMemory::instance();
        if (base) {
            (void)memory.map_pages(base, pages, nullptr);
            (void)memory.release_alias_region(base);
        }
        if (allocated) (void)memory.free_pages(pages, frames.data());
    }
};
}

bool verify_chad_host_fixture() noexcept {
    try {
        if (find_chad_export("XDspConnectWithMaximumStreamLimit") !=
                reinterpret_cast<FARPROC>(&XDspConnectWithMaximumStreamLimit) ||
            find_chad_export("XDspDisconnect") !=
                reinterpret_cast<FARPROC>(&XDspDisconnect) ||
            find_chad_export("XDspActivate") !=
                reinterpret_cast<FARPROC>(&XDspActivate) ||
            find_chad_export("XDspDeactivate") !=
                reinterpret_cast<FARPROC>(&XDspDeactivate) ||
            find_chad_export("XDspSubmitCommand") !=
                reinterpret_cast<FARPROC>(&XDspSubmitCommand) ||
            find_chad_export("XDspCreateClient") ||
            find_chad_export("xdspactivate"))
            return false;
        // Only recognized, verified handles can be disconnected. Do
        // not dereference unknown host or guest object pointers.
        const HostXDspStats before = host_xdsp_stats();
        auto& memory = memory::PhysicalMemory::instance();
        const size_t old_pages = memory.allocated_page_count();
        const size_t old_pins = memory.pinned_alias_count();
        uint64_t config = 123;
        constexpr uint32_t stream_hint = 30;
        HostXDspClient* client = reinterpret_cast<HostXDspClient*>(0x1234);
        if (XDspConnectWithMaximumStreamLimit(
                &config, stream_hint, 30, 128, 1, nullptr) != E_POINTER ||
            XDspConnectWithMaximumStreamLimit(
                nullptr, stream_hint, 30, 128, 1, &client) != E_INVALIDARG ||
            client ||
            XDspConnectWithMaximumStreamLimit(
                &config, stream_hint, 0, 128, 1, &client) != E_INVALIDARG ||
            XDspConnectWithMaximumStreamLimit(
                &config, stream_hint, 30, 128, 2, &client) != E_INVALIDARG ||
            XDspConnectWithMaximumStreamLimit(
                &config, stream_hint, 30, 0, 1, &client) != E_INVALIDARG ||
            XDspConnectWithMaximumStreamLimit(
                &config, stream_hint, 129, 128, 1, &client) != E_INVALIDARG)
            return false;
        if (XDspConnectWithMaximumStreamLimit(
                &config, stream_hint, 30, 128, 1, &client) != S_OK ||
            !client || host_xdsp_stats().live_clients !=
                            before.live_clients + 1)
            return false;
        if (XDspActivate(
                reinterpret_cast<void*>(uintptr_t{0x1234}),
                reinterpret_cast<void*>(uintptr_t{0x5678}), 1, 2, 3)
                    != E_NOTIMPL ||
            XDspDeactivate(reinterpret_cast<void*>(uintptr_t{0x1234}))
                    != E_NOTIMPL ||
            XDspSubmitCommand(client,
                reinterpret_cast<const void*>(uintptr_t{0x1234}))
                    != E_NOTIMPL ||
            XDspSubmitCommand(client, nullptr) != E_INVALIDARG ||
            XDspSubmitCommand(
                reinterpret_cast<HostXDspClient*>(uintptr_t{0x1234}),
                reinterpret_cast<const void*>(uintptr_t{0x1234}))
                    != E_INVALIDARG)
            return false;

        MappedXMemFixture pages;
        if (!pages.create()) return false;
        constexpr size_t kPage = memory::PhysicalMemory::page_bytes;
        constexpr uint32_t kSamples = 512;
        auto* const descriptor = pages.base + kPage - 16;
        auto* const input = reinterpret_cast<float*>(
            pages.base + 2 * kPage - 512);
        auto* const output = reinterpret_cast<float*>(
            pages.base + 4 * kPage - 512);
        std::array<float, kSamples> generated{};
        for (size_t i = 0; i < kSamples; ++i)
            generated[i] = (static_cast<float>(i % 9) - 4.0f) * 0.125f;
        std::copy(generated.begin(), generated.end(), input);
        std::fill_n(output, kSamples, 43.0f);

        HostXDspFirCommandV1 request{};
        request.input = input;
        request.output = output;
        request.samples = kSamples;
        request.command_id = 0xabcdef1234567890ull;
        std::memcpy(descriptor, &request, sizeof(request));
        // A generated command cannot succeed before an actual FIR
        // kernel has been installed in this host client.
        if (XDspSubmitCommand(client, descriptor) != E_NOT_VALID_STATE ||
            memory.pinned_alias_count() != old_pins ||
            !std::all_of(output, output + kSamples,
                [](float n) { return n == 43.0f; }))
            return false;
        const std::array<float, 1> identity{1.0f};
        if (configure_host_xdsp_fir(client, identity, 2) != S_OK)
            return false;

        // Never guess Xbox opcode meanings or parse an unverified
        // layout; even a well-formed but unknown XMem record is refused.
        request.magic = 5;
        std::memcpy(descriptor, &request, sizeof(request));
        if (XDspSubmitCommand(client, descriptor) != E_NOTIMPL)
            return false;
        request.magic = kHostXDspFirCommandMagic;
        std::memcpy(descriptor, &request, sizeof(request));
        auto* const second_header = pages.base + kPage;
        if (memory.map_pages(second_header, 1, nullptr) != second_header ||
            XDspSubmitCommand(client, descriptor) != E_NOTIMPL ||
            memory.pinned_alias_count() != old_pins ||
            memory.map_pages(second_header, 1,
                &pages.frames[1]) != second_header)
            return false;
        auto* const input_second = pages.base + 2 * kPage;
        if (memory.map_pages(input_second, 1, nullptr) != input_second ||
            XDspSubmitCommand(client, descriptor) != E_NOTIMPL ||
            memory.pinned_alias_count() != old_pins ||
            memory.map_pages(input_second, 1,
                &pages.frames[2]) != input_second)
            return false;
        auto* const last_page = pages.base + 4 * kPage;
        if (memory.map_pages(last_page, 1, nullptr) != last_page ||
            XDspSubmitCommand(client, descriptor) != E_NOTIMPL ||
            memory.pinned_alias_count() != old_pins ||
            memory.map_pages(last_page, 1,
                &pages.frames[4]) != last_page)
            return false;

        // Real host FIR work and exact cross-page sample writeback.
        if (XDspSubmitCommand(client, descriptor) != S_OK ||
            !std::equal(generated.begin(), generated.end(), output) ||
            memory.pinned_alias_count() != old_pins ||
            host_xdsp_stats().host_fir_commands !=
                before.host_fir_commands + 1)
            return false;
        // A stale, foreign or disconnected client cannot submit more
        // commands, and the registry will never recycle its address.
        if (XDspDisconnect(client) != S_OK ||
            XDspDisconnect(client) != E_INVALIDARG ||
            XDspSubmitCommand(client, descriptor) != E_INVALIDARG ||
            configure_host_xdsp_fir(client, identity, 2) != E_INVALIDARG)
            return false;
        if (memory.map_pages(pages.base, pages.pages, nullptr) !=
                pages.base ||
            !memory.release_alias_region(pages.base) ||
            !memory.free_pages(pages.pages, pages.frames.data()))
            return false;
        pages.base = nullptr;
        pages.allocated = false;
        const auto after = host_xdsp_stats();
        return after.registered_clients == before.registered_clients + 1 &&
               after.live_clients == before.live_clients &&
               memory.allocated_page_count() == old_pages &&
               memory.pinned_alias_count() == old_pins;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::audio
