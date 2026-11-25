#include "apu_memory.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>
#include <type_traits>
#include <vector>

namespace scarlett::audio {
namespace {
using AllocFn = HRESULT (WINAPI*)(void**, void*, uint32_t, uint32_t) noexcept;
using FreeFn = void (WINAPI*)(void*) noexcept;
static_assert(std::is_same_v<decltype(&ApuAlloc), AllocFn>);
static_assert(std::is_same_v<decltype(&ApuFree), FreeFn>);

bool test_one(AllocFn alloc, FreeFn release, uint32_t bytes,
              uint32_t align) noexcept {
    void* memory = reinterpret_cast<void*>(uintptr_t{0x1357});
    if (alloc(&memory, nullptr, bytes, align) != S_OK || !memory)
        return false;
    const auto snap = host_apu_allocation_stats();
    bool correct = (reinterpret_cast<uintptr_t>(memory) %
                    (align > alignof(void*) ? align : alignof(void*))) == 0;
    const auto* buffer = static_cast<const uint8_t*>(memory);
    for (uint32_t i = 0; i < bytes; ++i)
        correct &= buffer[i] == 0;
    // Real writable CPU-backed memory, not a dummy success pointer.
    auto* writable = static_cast<uint8_t*>(memory);
    writable[0] = 0x6b;
    if (bytes > 1)
        writable[bytes - 1] = 0x96;
    correct &= writable[0] == 0x6b &&
               (bytes == 1 || writable[bytes - 1] == 0x96);
    release(writable + 1); // not the allocation base
    const auto after_interior = host_apu_allocation_stats();
    correct &= snap.allocated_bytes == after_interior.allocated_bytes &&
               snap.live_allocations == after_interior.live_allocations;
    release(memory);
    release(memory); // must not double-free
    return correct;
}
} // namespace

bool verify_host_apu_allocation_fixtures() noexcept {
    try {
        const auto alloc_export = find_acphal_export("ApuAlloc");
        const auto free_export = find_acphal_export("ApuFree");
        if (alloc_export != reinterpret_cast<FARPROC>(&ApuAlloc) ||
            free_export != reinterpret_cast<FARPROC>(&ApuFree) ||
            find_acphal_export("ApuCreateHeap") ||
            find_acphal_export("AcpHalCreate") ||
            find_acphal_export("ApuMapVirtualAddress") ||
            find_acphal_export("apualloc") ||
            find_acphal_export(""))
            return false;

        const auto alloc = reinterpret_cast<AllocFn>(alloc_export);
        const auto release = reinterpret_cast<FreeFn>(free_export);
        const auto initial = host_apu_allocation_stats();

        // Verify all errors null the output and never reserve host memory.
        if (alloc(nullptr, nullptr, 64, 0x800) != E_POINTER)
            return false;
        const std::array<uint32_t, 6> bad_align{
            0u, 1u, 2u, 3u, 7u, 131072u};
        for (uint32_t alignment : bad_align) {
            void* out = reinterpret_cast<void*>(uintptr_t{0x1234});
            if (alloc(&out, nullptr, 64, alignment) != E_INVALIDARG ||
                out)
                return false;
        }
        for (uint32_t bytes : {0u, kHostApuMaxSingleAllocation + 1u,
                               0xffffffffu}) {
            void* out = reinterpret_cast<void*>(uintptr_t{0x1234});
            if (alloc(&out, nullptr, bytes, 2048u) != E_INVALIDARG ||
                out)
                return false;
        }
        void* unsupported = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (alloc(&unsupported, reinterpret_cast<void*>(uintptr_t{1}),
                  64u, 2048u) != E_NOTIMPL || unsupported)
            return false;

        for (auto [bytes, alignment] : std::array<std::pair<uint32_t,uint32_t>, 8>{{
                {1u,4u},{17u,8u},{32u,16u},{257u,32u},
                {1023u,64u},{4097u,2048u},{8193u,4096u},
                {65537u,65536u}}}) {
            if (!test_one(alloc, release, bytes, alignment))
                return false;
        }

        std::array<uint8_t,64> foreign{};
        foreign.fill(0xa5u);
        release(foreign.data());
        release(nullptr);
        for (uint8_t b : foreign)
            if (b != 0xa5u)
                return false;

        // Concurrent allocations must preserve owner bookkeeping.
        std::atomic<bool> healthy{true};
        std::vector<std::jthread> threads;
        threads.reserve(4);
        for (int tid = 0; tid < 4; ++tid) {
            threads.emplace_back([=,&healthy] {
                for (int i = 0; i < 128; ++i) {
                    void* memory = nullptr;
                    if (alloc(&memory, nullptr, 96u + (i % 31),
                              2048u) != S_OK || !memory) {
                        healthy.store(false);
                        return;
                    }
                    if (reinterpret_cast<uintptr_t>(memory) % 2048u != 0)
                        healthy.store(false);
                    release(memory);
                }
            });
        }
        for (auto& thread : threads)
            thread.join();
        const auto final = host_apu_allocation_stats();
        return healthy.load() &&
               final.allocated_bytes == initial.allocated_bytes &&
               final.live_allocations == initial.live_allocations;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::audio
