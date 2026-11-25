#include "xg_field_projection.h"
#include <cstdint>
#include <cstring>

namespace scarlett::analysis {
namespace {
uint16_t read_le16(const uint8_t* ptr) noexcept {
    return static_cast<uint16_t>(ptr[0]) |
        static_cast<uint16_t>(static_cast<unsigned>(ptr[1]) << 8);
}

void write_le(std::span<uint8_t> buffer, size_t index,
              uint64_t value, size_t width) noexcept {
    for (size_t i = 0; i < width; ++i) {
        buffer[index + i] = static_cast<uint8_t>(value & 0xffu);
        value >>= 8;
    }
}

bool spans_overlap(std::span<const uint8_t> a,
                   std::span<uint8_t> b) noexcept {
    // Pure address comparison; no pointer subtraction on unrelated objects.
    const auto start_a = reinterpret_cast<uintptr_t>(a.data());
    const auto start_b = reinterpret_cast<uintptr_t>(b.data());
    return (start_a <= start_b && start_b - start_a < a.size()) ||
           (start_b <= start_a && start_a - start_b < b.size());
}
} // namespace

XgFieldProjectionStatus decode_xg_observed_fields(
    std::span<const uint8_t> compact,
    XgObservedCompactFields& result) noexcept {
    if (compact.size() < kXgCompactObservedInputExtent)
        return XgFieldProjectionStatus::truncated_input;
    // In the observed title-local helper, these two values branch away
    // before the common stores. Their alternate layouts are unverified.
    if (compact[6] == 0 || compact[6] == 3)
        return XgFieldProjectionStatus::unsupported_branch;

    XgObservedCompactFields next;
    next.source_00 = read_le16(compact.data() + 0x00);
    next.source_02 = read_le16(compact.data() + 0x02);
    next.source_04 = read_le16(compact.data() + 0x04);
    next.source_07 = compact[0x07];
    next.source_0a = compact[0x0a];
    result = next;
    return XgFieldProjectionStatus::ok;
}

XgFieldProjectionStatus project_xg_observed_fields(
    std::span<const uint8_t> compact,
    std::span<uint8_t> prepared) noexcept {
    XgObservedCompactFields extracted;
    const auto status = decode_xg_observed_fields(compact, extracted);
    if (status != XgFieldProjectionStatus::ok)
        return status;
    if (prepared.size() < kXgPreparedObservedOutputExtent)
        return XgFieldProjectionStatus::truncated_output;
    if (spans_overlap(compact, prepared))
        return XgFieldProjectionStatus::overlapping_buffers;

    // These five transfers were directly visible in a single title-side
    // helper path. They are not sufficient to call XGCreateTextureComputer.
    write_le(prepared, 0x10, extracted.source_00, 8);
    write_le(prepared, 0x18, extracted.source_02, 4);
    write_le(prepared, 0x1c, extracted.source_04, 2);
    write_le(prepared, 0x1e, extracted.source_0a, 2);
    write_le(prepared, 0x20, extracted.source_07, 4);
    return XgFieldProjectionStatus::ok;
}
} // namespace scarlett::analysis
