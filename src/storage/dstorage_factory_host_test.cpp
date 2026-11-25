#include "dstorage_factory_host.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>

namespace scarlett::storage {
namespace {
using GetFactoryFn = HRESULT (WINAPI*)(REFIID, void**) noexcept;
using SetStagingFn = HRESULT (STDMETHODCALLTYPE*)(
    HostDStorageFactoryX2*, uint32_t) noexcept;
static_assert(std::is_same_v<decltype(&DStorageGetFactory), GetFactoryFn>);

constexpr uint32_t mib = 1024u * 1024u;
struct ComRelease {
    HostDStorageFactoryX2* object = nullptr;
    ~ComRelease() { if (object) object->Release(); }
};
bool check_stage(HostDStorageFactoryX2* factory, uint32_t size) {
    return factory->SetStagingBufferSize(size) == S_OK &&
           host_dstorage_staging_bytes() == size &&
           (size == 0 || verify_dstorage_staging_pages_for_fixture());
}
}
bool verify_dstorage_factory_fixture() noexcept {
    try {
        if (find_dstorage_export("DStorageGetFactory") !=
                reinterpret_cast<FARPROC>(&DStorageGetFactory) ||
            find_dstorage_export("dstoragegetfactory") ||
            find_dstorage_export("DStorageGetFactoryOrdinal") ||
            find_dstorage_export(""))
            return false;

        const GUID invalid{0xabcdefab,0xcdef,0x1234,{1,2,3,4,5,6,7,8}};
        void* out = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (DStorageGetFactory(kObservedXboxDStorageFactoryIid, nullptr)
                != E_POINTER ||
            DStorageGetFactory(invalid, &out) != E_NOINTERFACE ||
            out != nullptr ||
            host_dstorage_staging_bytes() != 0)
            return false;

        const auto get = reinterpret_cast<GetFactoryFn>(
            find_dstorage_export("DStorageGetFactory"));
        if (get(kObservedXboxDStorageFactoryIid, &out) != S_OK ||
            !out || host_dstorage_staging_bytes() != kDefaultStagingBytes ||
            !verify_dstorage_staging_pages_for_fixture())
            return false;
        ComRelease owned{static_cast<HostDStorageFactoryX2*>(out)};
        // Exercise exact documented COM vtable layout as actual x64
        // pointers (not merely directly calling a C++ method).
        auto* table = *reinterpret_cast<void***>(owned.object);
        const auto staging_slot =
            reinterpret_cast<SetStagingFn>(table[8]);
        if (!staging_slot ||
            staging_slot(owned.object, 32*mib) != S_OK ||
            host_dstorage_staging_bytes() != 32*mib)
            return false;

        // Static singleton COM identity, IUnknown identity and correct
        // reference ownership through QI, AddRef and Release.
        ComRelease second{};
        if (get(kObservedXboxDStorageFactoryIid,
                reinterpret_cast<void**>(&second.object)) != S_OK ||
            second.object != owned.object)
            return false;
        IUnknown* unknown = nullptr;
        if (owned.object->QueryInterface(
                IID_IUnknown, reinterpret_cast<void**>(&unknown)) != S_OK ||
            unknown != static_cast<IUnknown*>(owned.object))
            return false;
        unknown->Release();
        out = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (owned.object->QueryInterface(invalid, &out) != E_NOINTERFACE ||
            out != nullptr ||
            owned.object->QueryInterface(IID_IUnknown, nullptr) != E_POINTER)
            return false;

        // Real memory allocation and mutable staging budget, not fake
        // S_OK. Only public size set {0,16,20,24,28,32} MiB accepted.
        for (uint32_t size : std::array<uint32_t,7>{
                 0,16*mib,20*mib,24*mib,28*mib,32*mib,0})
            if (!check_stage(owned.object, size))
                return false;
        for (uint32_t size : std::array<uint32_t,5>{
                 1,2*mib,8*mib,33*mib,0xffffffffu}) {
            const uint32_t before = host_dstorage_staging_bytes();
            if (owned.object->SetStagingBufferSize(size) != E_INVALIDARG ||
                host_dstorage_staging_bytes() != before)
                return false;
        }

        // Guest file/queue calls fail explicitly; the independently
        // verified status-array interface now has real COM state.
        out = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (owned.object->CreateQueue(
                nullptr, invalid, &out) != E_NOTIMPL || out)
            return false;
        out = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (owned.object->CreateQueue1(
                nullptr, invalid, &out) != E_NOTIMPL || out)
            return false;
        out = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (owned.object->CreateStatusArray(
                4, "generated", invalid, &out) != E_NOINTERFACE || out)
            return false;
        out = reinterpret_cast<void*>(uintptr_t{0x1234});
        if (owned.object->OpenFile(
                L"reports\\never-open-game.bin", invalid, &out)
                != E_NOTIMPL || out)
            return false;
        if (owned.object->CreateQueue(
                nullptr, invalid, nullptr) != E_POINTER ||
            owned.object->SetCpuAffinity(1) != E_NOTIMPL)
            return false;
        std::array<uint8_t, 96> stats{};
        stats.fill(0x9D);
        owned.object->SetDebugFlags(0x55);
        owned.object->GetStats(stats.data());
        for (uint8_t b : stats)
            if (b != 0x9D)
                return false;

        // Multiple simultaneous COM callers can fetch the same static
        // factory and resize safely. After each worker, the final size
        // is always an allowed value; no object memory is freed on Release.
        if (!check_stage(owned.object, 16*mib)) return false;
        std::atomic<bool> passed{true};
        std::vector<std::jthread> workers;
        for (unsigned t=0; t<4; ++t) {
            workers.emplace_back([&, t]{
                for (unsigned i=0; i<48; ++i) {
                    ComRelease local{};
                    if (get(kObservedXboxDStorageFactoryIid,
                            reinterpret_cast<void**>(&local.object))
                            != S_OK || local.object != owned.object) {
                        passed.store(false);
                        return;
                    }
                    // Keep simultaneous allocations moderate, while the
                    // interleaved per-thread changes test synchronized
                    // memory ownership and COM references.
                    if (local.object->SetStagingBufferSize(
                            ((i + t) % 2 == 0) ? 0 : 16*mib) != S_OK)
                        passed.store(false);
                }
            });
        }
        for (auto& worker: workers) worker.join();
        if (!passed.load()) return false;
        const uint32_t final_size = host_dstorage_staging_bytes();
        if (final_size != 0 && final_size != 16*mib)
            return false;
        return check_stage(owned.object, kDefaultStagingBytes);
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::storage
