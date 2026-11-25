#include "apu_memory.h"
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
using CheckFn = BOOL (WINAPI*)(const void*, uint32_t) noexcept;
using HostToApuFn = uint32_t (WINAPI*)(const void*) noexcept;
using ApuToHostFn = void* (WINAPI*)(uint32_t) noexcept;
using AllocFn = HRESULT (WINAPI*)(void**, uint32_t*, uint32_t, uint32_t,
                                   uint32_t) noexcept;
using FreeFn = void (WINAPI*)(void*) noexcept;
static_assert(std::is_same_v<decltype(&ApuIsVirtualAddressValid), CheckFn>);
static_assert(std::is_same_v<decltype(&ApuMapVirtualAddress), HostToApuFn>);
static_assert(std::is_same_v<decltype(&ApuMapApuAddress), ApuToHostFn>);
} // namespace

bool verify_host_apu_address_fixtures() noexcept {
    try {
        const FARPROC check_proc = find_acphal_export(
            "ApuIsVirtualAddressValid");
        const FARPROC to_apu_proc = find_acphal_export(
            "ApuMapVirtualAddress");
        const FARPROC to_host_proc = find_acphal_export(
            "ApuMapApuAddress");
        if (check_proc != reinterpret_cast<FARPROC>(
                              &ApuIsVirtualAddressValid) ||
            to_apu_proc != reinterpret_cast<FARPROC>(
                               &ApuMapVirtualAddress) ||
            to_host_proc != reinterpret_cast<FARPROC>(
                                &ApuMapApuAddress) ||
            find_acphal_export("ApuMapVirtualAddressExtra") ||
            find_acphal_export("ApuMapApuAddressOrdinal") ||
            find_acphal_export("apumapvirtualaddress") ||
            find_acphal_export("AcpHalCreate") !=
                reinterpret_cast<FARPROC>(&AcpHalCreate) ||
            find_acphal_export("ApuMapPhysicalAddress"))
            return false;
        auto valid = reinterpret_cast<CheckFn>(check_proc);
        auto to_apu = reinterpret_cast<HostToApuFn>(to_apu_proc);
        auto to_host = reinterpret_cast<ApuToHostFn>(to_host_proc);
        auto alloc = reinterpret_cast<AllocFn>(find_acphal_export("ApuAlloc"));
        auto release = reinterpret_cast<FreeFn>(find_acphal_export("ApuFree"));
        if (!alloc || !release)
            return false;

        const auto before = host_apu_allocation_stats();
        if (valid(nullptr, 1) || to_apu(nullptr) || to_host(0))
            return false;
        std::array<uint8_t, 16> foreign{};
        if (valid(foreign.data(), 1) ||
            to_apu(foreign.data()) ||
            to_host(0xffffffffu))
            return false;

        void* a = nullptr;
        void* b = nullptr;
        if (alloc(&a, nullptr, 257u, 4096u, 0) != S_OK || !a)
            return false;
        if (alloc(&b, nullptr, 513u, 65536u, 0) != S_OK || !b) {
            release(a);
            return false;
        }
        bool good = true;
        auto* a_bytes = static_cast<uint8_t*>(a);
        auto* b_bytes = static_cast<uint8_t*>(b);
        const uint32_t a_addr = to_apu(a);
        const uint32_t b_addr = to_apu(b);
        good &= a_addr != 0 && b_addr != 0 &&
                (a_addr % 4096u) == 0 &&
                (b_addr % 65536u) == 0 &&
                b_addr > a_addr + 257u;
        good &= valid(a, 257u) == TRUE &&
                valid(a, 0) == FALSE &&
                valid(a, 258u) == FALSE &&
                valid(a_bytes + 256, 1u) == TRUE &&
                valid(a_bytes + 256, 2u) == FALSE &&
                valid(a_bytes + 257, 1u) == FALSE &&
                valid(b, 513u) == TRUE &&
                valid(b_bytes + 512, 1u) == TRUE &&
                valid(b_bytes + 512, 2u) == FALSE;

        // End-to-end address round-trips, including interior byte offsets.
        for (uint32_t offset = 0; offset < 257u; ++offset) {
            good &= to_apu(a_bytes + offset) == a_addr + offset &&
                    to_host(a_addr + offset) == a_bytes + offset;
        }
        for (uint32_t offset = 0; offset < 513u; ++offset) {
            good &= to_apu(b_bytes + offset) == b_addr + offset &&
                    to_host(b_addr + offset) == b_bytes + offset;
        }
        good &= to_apu(a_bytes + 257) == 0 &&
                to_apu(b_bytes + 513) == 0 &&
                to_host(a_addr - 1) == nullptr &&
                to_host(a_addr + 257u) == nullptr &&
                to_host(b_addr - 1u) == nullptr &&
                to_host(b_addr + 513u) == nullptr;

        a_bytes[0] = 0x5au;
        a_bytes[256] = 0xa5u;
        good &= *static_cast<uint8_t*>(to_host(a_addr)) == 0x5au &&
                *static_cast<uint8_t*>(to_host(a_addr + 256u)) == 0xa5u;
        // The optional 32-bit output is a real, owned APU address,
        // independently round-trippable to the same CPU allocation.
        uint32_t optional_apu = 0xaabbccddu;
        void* optional_cpu = nullptr;
        const HRESULT optional_status = alloc(
            &optional_cpu, &optional_apu, 16u, 4096u, 0);
        good &= optional_status == S_OK && optional_cpu &&
                optional_apu != 0 &&
                to_apu(optional_cpu) == optional_apu &&
                to_host(optional_apu) == optional_cpu &&
                (optional_apu % 4096u) == 0;
        release(optional_cpu);
        good &= to_host(optional_apu) == nullptr;

        // On unsupported flags, neither output may masquerade as success.
        void* refused_cpu = reinterpret_cast<void*>(uintptr_t{0x1234});
        uint32_t refused_apu = 0xaabbccddu;
        good &= alloc(&refused_cpu, &refused_apu,
                      16u, 4096u, 1) == E_NOTIMPL &&
                refused_cpu == nullptr &&
                refused_apu == 0;

        release(a);
        good &= to_host(a_addr) == nullptr &&
                to_host(a_addr + 256u) == nullptr &&
                to_apu(a) == 0 &&
                valid(a, 1) == FALSE &&
                to_host(b_addr) == b;
        release(b);
        good &= to_host(b_addr) == nullptr &&
                to_apu(b) == 0;

        // Fresh APU VA assignments are monotonic. An old freed address
        // cannot resolve a new allocation even if _aligned_malloc reuses
        // the exact same host pointer.
        void* reuse = nullptr;
        if (alloc(&reuse, nullptr, 257u, 4096u, 0) != S_OK || !reuse)
            return false;
        const uint32_t reuse_addr = to_apu(reuse);
        good &= reuse_addr > b_addr + 513u &&
                to_host(a_addr) == nullptr &&
                to_host(b_addr) == nullptr;
        release(reuse);

        std::atomic<bool> concurrent_pass{true};
        std::vector<std::jthread> threads;
        threads.reserve(4);
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (uint32_t i = 0; i < 128; ++i) {
                    void* pointer = nullptr;
                    const uint32_t bytes = 128u + i + uint32_t(t);
                    if (alloc(&pointer, nullptr, bytes, 2048u, 0) != S_OK ||
                        !pointer) {
                        concurrent_pass.store(false);
                        return;
                    }
                    const auto address = to_apu(pointer);
                    auto* base = static_cast<uint8_t*>(pointer);
                    if (address == 0 || (address % 4096u) != 0 ||
                        valid(pointer, bytes) != TRUE ||
                        to_host(address) != pointer ||
                        to_host(address + bytes - 1u) !=
                            base + bytes - 1u)
                        concurrent_pass.store(false);
                    release(pointer);
                    if (to_host(address) != nullptr)
                        concurrent_pass.store(false);
                }
            });
        }
        for (auto& thread : threads)
            thread.join();

        const auto after = host_apu_allocation_stats();
        return good && concurrent_pass.load() &&
               after.live_allocations == before.live_allocations &&
               after.allocated_bytes == before.allocated_bytes;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::audio
