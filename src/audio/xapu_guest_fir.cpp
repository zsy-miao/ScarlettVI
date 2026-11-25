#include "xapu_host.h"
#include "../runtime/physical_memory.h"
#include <array>
#include <limits>
#include <vector>

namespace scarlett::audio {
namespace {
// A transfer may span 64KiB boundaries. Pins are acquired after the
// PhysicalMemory range resolver verifies all mapped frames, then each
// identity is rechecked during pin admission. A partial failure rolls
// back every previously acquired pin without touching the output.
class PinnedAudioCpuDestination final {
public:
    PinnedAudioCpuDestination() = default;
    PinnedAudioCpuDestination(const PinnedAudioCpuDestination&) = delete;
    PinnedAudioCpuDestination& operator=(
        const PinnedAudioCpuDestination&) = delete;
    ~PinnedAudioCpuDestination() noexcept {
        auto& memory = memory::PhysicalMemory::instance();
        for (size_t n = count_; n != 0; --n)
            (void)memory.unpin_mapped_alias(
                reinterpret_cast<void*>(base_ +
                    (n - 1) * memory::PhysicalMemory::page_bytes),
                frames_[n - 1]);
    }
    bool pin(void* destination, size_t bytes) noexcept {
        auto& memory = memory::PhysicalMemory::instance();
        memory::PhysicalMemory::MappedAliasSpan found{};
        if (!memory.resolve_mapped_alias_span(
                destination, bytes, found)) return false;
        base_ = reinterpret_cast<uintptr_t>(destination) &
                ~static_cast<uintptr_t>(memory::PhysicalMemory::page_bytes - 1);
        frames_ = found.frames;
        for (size_t i = 0; i < found.count; ++i) {
            if (!memory.pin_mapped_alias(
                    reinterpret_cast<void*>(base_ +
                        i * memory::PhysicalMemory::page_bytes),
                    frames_[i]))
                return false;
            ++count_;
        }
        return true;
    }
private:
    uintptr_t base_ = 0;
    std::array<uintptr_t,
        memory::PhysicalMemory::MappedAliasSpan::kMaxFrames> frames_{};
    size_t count_ = 0;
};
} // namespace

extern "C" HRESULT WINAPI XApuEnqueueCommand(
    HostXApuConnection* connection,
    const void* command, uint32_t flags) noexcept {
    if (!connection || !command || flags != 0)
        return E_INVALIDARG;

    auto& memory = memory::PhysicalMemory::instance();
    HostXApuFirCommandV1 snapshot{};
    // No access to arbitrary guest heap, stack, or unknown Xbox command
    // memory. This function recognizes only our host-generated payload
    // when the entire fixed-size header is in a mapped XMem CPU alias.
    if (!memory.snapshot_mapped_alias(
            command, sizeof(snapshot), &snapshot))
        return E_NOTIMPL;
    if (snapshot.magic != kHostXApuFirCommandMagic ||
        snapshot.version != 1 || snapshot.reserved ||
        snapshot.reserved2 || snapshot.reserved3)
        return E_NOTIMPL;

    constexpr uint32_t kMaxSamples = 16 * 1024;
    if (!snapshot.input || !snapshot.output ||
        !snapshot.samples || snapshot.samples > kMaxSamples ||
        (reinterpret_cast<uintptr_t>(snapshot.input) & (alignof(float)-1)) ||
        (reinterpret_cast<uintptr_t>(snapshot.output) & (alignof(float)-1)))
        return E_INVALIDARG;

    const size_t bytes = static_cast<size_t>(snapshot.samples) * sizeof(float);
    try {
        // Snapshot the input while all its XMem mappings are held under
        // the physical registry mutex; a producer may release that
        // mapping immediately afterwards. Avoid processing stale VAs.
        std::vector<float> input(snapshot.samples);
        if (!memory.snapshot_mapped_alias(
                snapshot.input, bytes, input.data()))
            return E_NOTIMPL;
        // Hold all output alias pins across DSP processing AND the
        // transactionally enqueued host completion receipt.
        PinnedAudioCpuDestination pins;
        if (!pins.pin(snapshot.output, bytes))
            return E_NOTIMPL;

        // Existing tested FIR state / result FIFO does real work under
        // its connection lock. Unsupported Xbox command structures
        // never reach the DSP processor and cannot report success.
        return submit_host_xapu_fir(
            connection, input,
            std::span<float>(snapshot.output, snapshot.samples),
            snapshot.command_id);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}
} // namespace scarlett::audio
