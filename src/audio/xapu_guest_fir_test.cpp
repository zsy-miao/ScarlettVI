#include "xapu_host.h"
#include "../runtime/physical_memory.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace scarlett::audio {
namespace {
struct FiveMappedXMemPages {
    static constexpr size_t kCount = 5;
    std::array<uintptr_t, kCount> frames{};
    uint8_t* alias = nullptr;
    bool allocated = false;

    bool create() noexcept {
        auto& mem = memory::PhysicalMemory::instance();
        uintptr_t count = kCount;
        if (!mem.allocate_pages(MEM_64K_PAGES, &count, frames.data()) ||
            count != kCount)
            return false;
        allocated = true;
        alias = static_cast<uint8_t*>(mem.reserve_alias_region(
            nullptr, kCount * mem.page_bytes));
        return alias && mem.map_pages(alias, kCount, frames.data()) == alias;
    }

    ~FiveMappedXMemPages() noexcept {
        auto& mem = memory::PhysicalMemory::instance();
        if (alias) {
            (void)mem.map_pages(alias, kCount, nullptr);
            (void)mem.release_alias_region(alias);
        }
        if (allocated) (void)mem.free_pages(kCount, frames.data());
    }
};

uint32_t receipt_checksum(const float* output, size_t samples) noexcept {
    uint32_t sum = 2166136261u;
    for (size_t i = 0; i < samples; ++i) {
        sum ^= std::bit_cast<uint32_t>(output[i]);
        sum *= 16777619u;
    }
    return sum;
}
} // namespace

bool verify_host_xapu_guest_fir_fixture() noexcept {
    try {
        using Fn = HRESULT (WINAPI*)(
            HostXApuConnection*, const void*, uint32_t) noexcept;
        if (find_xapu_export("XApuEnqueueCommand") !=
                reinterpret_cast<FARPROC>(&XApuEnqueueCommand))
            return false;
        const Fn enqueue = reinterpret_cast<Fn>(
            find_xapu_export("XApuEnqueueCommand"));
        if (!enqueue) return false;
        auto& memory = memory::PhysicalMemory::instance();
        const size_t old_frames = memory.allocated_page_count();
        const size_t old_pins = memory.pinned_alias_count();
        const HostXApuStats old_stats = host_xapu_stats();
        FiveMappedXMemPages mapped;
        if (!mapped.create()) return false;
        uint64_t desc = 0x1234, callbacks = 0x4321;
        HostXApuConnection* connection = nullptr;
        if (XApuConnect(&desc, &callbacks, &connection) != S_OK ||
            !connection)
            return false;

        constexpr size_t page = memory::PhysicalMemory::page_bytes;
        constexpr uint32_t samples = 1024;
        // Request lives at the end of page zero and crosses page one.
        auto* const guest_command = mapped.alias + page - 16;
        // Input and output each cross *different* mapped XMem pages.
        auto* const input = reinterpret_cast<float*>(
            mapped.alias + 2 * page - 512);
        auto* const output = reinterpret_cast<float*>(
            mapped.alias + 4 * page - 1024);
        std::array<float, samples> generated{};
        for (size_t i = 0; i < samples; ++i) {
            generated[i] = static_cast<float>(
                static_cast<int>(i % 37) - 18) * 0.03125f;
        }
        std::copy(generated.begin(), generated.end(), input);
        std::fill_n(output, samples, 77.0f);
        HostXApuFirCommandV1 command{};
        command.input = input;
        command.output = output;
        command.samples = samples;
        constexpr uint64_t kCommandId = 0xfedcba9876543210ull;
        command.command_id = kCommandId;
        std::memcpy(guest_command, &command, sizeof(command));
        const void* const untrusted =
            reinterpret_cast<const void*>(uintptr_t{0x1234});
        if (enqueue(connection, untrusted, 0) != E_NOTIMPL ||
            enqueue(connection, &command, 0) != E_NOTIMPL ||
            enqueue(connection, guest_command, 1) != E_INVALIDARG ||
            XApuGetResultCount(connection, 0) != 0 ||
            memory.pinned_alias_count() != old_pins)
            return false;

        // The command is recognized but DSP cannot run without a real
        // configured kernel. No fake receipt and no output bytes.
        if (enqueue(connection, guest_command, 0) != E_NOT_VALID_STATE ||
            XApuGetResultCount(connection, 0) != 0 ||
            !std::all_of(output, output + samples,
                [](float f) { return f == 77.0f; }) ||
            memory.pinned_alias_count() != old_pins)
            return false;

        const std::array<float, 1> identity{1.0f};
        if (configure_host_xapu_fir(connection, identity, 2) != S_OK)
            return false;

        // Reject unknown Xbox command opcodes, unowned input and output,
        // and a partially unmapped destination (with no partial output).
        command.magic = 1;
        std::memcpy(guest_command, &command, sizeof(command));
        if (enqueue(connection, guest_command, 0) != E_NOTIMPL)
            return false;
        command.magic = kHostXApuFirCommandMagic;
        // The 48-byte command header crosses pages zero and one. The
        // source-page loss must fail before copying any command bytes.
        uint8_t* const request_page_one = mapped.alias + page;
        std::memcpy(guest_command, &command, sizeof(command));
        if (memory.map_pages(request_page_one, 1, nullptr) !=
                request_page_one ||
            enqueue(connection, guest_command, 0) != E_NOTIMPL ||
            memory.map_pages(request_page_one, 1,
                &mapped.frames[1]) != request_page_one ||
            memory.pinned_alias_count() != old_pins)
            return false;
        // The sample payload also crosses from page one to page two.
        // A partially missing input is rejected without processing.
        uint8_t* const input_second_page = mapped.alias + 2 * page;
        if (memory.map_pages(input_second_page, 1, nullptr) !=
                input_second_page ||
            enqueue(connection, guest_command, 0) != E_NOTIMPL ||
            memory.map_pages(input_second_page, 1,
                &mapped.frames[2]) != input_second_page ||
            memory.pinned_alias_count() != old_pins)
            return false;
        command.input = reinterpret_cast<const float*>(uintptr_t{0x5678});
        std::memcpy(guest_command, &command, sizeof(command));
        if (enqueue(connection, guest_command, 0) != E_NOTIMPL)
            return false;
        command.input = input;
        command.output = reinterpret_cast<float*>(uintptr_t{0x5678});
        std::memcpy(guest_command, &command, sizeof(command));
        if (enqueue(connection, guest_command, 0) != E_NOTIMPL)
            return false;
        command.output = output;
        std::memcpy(guest_command, &command, sizeof(command));
        uint8_t* const last_page = mapped.alias + 4 * page;
        if (memory.map_pages(last_page, 1, nullptr) != last_page ||
            enqueue(connection, guest_command, 0) != E_NOTIMPL ||
            memory.pinned_alias_count() != old_pins ||
            memory.map_pages(last_page, 1, &mapped.frames[4]) != last_page ||
            XApuGetResultCount(connection, 0) != 0)
            return false;

        // Actual linked guest import, actual host convolution, real
        // mapped CPU destination and one completed host receipt.
        if (enqueue(connection, guest_command, 0) != S_OK ||
            !std::equal(generated.begin(), generated.end(), output) ||
            memory.pinned_alias_count() != old_pins ||
            XApuGetResultCount(connection, 0) != 1)
            return false;
        XApuResultEnvelope result{};
        if (XApuDequeueResult(connection, &result) != S_OK ||
            result.words[0] != kHostXApuFirResultMagic ||
            result.words[1] != static_cast<uint32_t>(kCommandId) ||
            result.words[2] != static_cast<uint32_t>(kCommandId >> 32) ||
            result.words[3] != samples / 2 ||
            result.words[4] != 2 ||
            result.words[5] != receipt_checksum(output, samples) ||
            result.words[6] != samples / 2 ||
            XApuGetResultCount(connection, 0) != 0)
            return false;

        // Full host receipt queue must reject commands before processing,
        // leaving the destination unchanged and releasing all page pins.
        for (size_t i = 0; i < kHostXApuResultCapacity; ++i) {
            if (!inject_synthetic_xapu_result_for_fixture(
                    connection, XApuResultEnvelope{}))
                return false;
        }
        std::fill_n(output, samples, -13.0f);
        if (enqueue(connection, guest_command, 0) != kHostXApuQueueFull ||
            !std::all_of(output, output + samples,
                [](float f) { return f == -13.0f; }) ||
            memory.pinned_alias_count() != old_pins ||
            XApuGetResultCount(connection, 0) != kHostXApuResultCapacity)
            return false;
        if (XApuDisconnect(connection) != S_OK ||
            enqueue(connection, guest_command, 0) != E_INVALIDARG ||
            XApuGetResultCount(connection, 0) != 0)
            return false;

        // Do not release memory unless all expected pins have gone.
        if (memory.map_pages(mapped.alias, mapped.kCount, nullptr) !=
                mapped.alias ||
            !memory.release_alias_region(mapped.alias) ||
            !memory.free_pages(mapped.kCount, mapped.frames.data()))
            return false;
        mapped.alias = nullptr;
        mapped.allocated = false;
        const HostXApuStats end = host_xapu_stats();
        return memory.allocated_page_count() == old_frames &&
            memory.pinned_alias_count() == old_pins &&
            end.active_connections == old_stats.active_connections &&
            end.pending_results == old_stats.pending_results;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::audio
