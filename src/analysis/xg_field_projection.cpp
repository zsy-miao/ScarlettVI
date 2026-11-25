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
    // Both category 0 and category 3 also rejoin the common store block;
    // their incoming values differ and are explicitly handled below.
    XgObservedCompactFields next;
    next.category_06 = compact[6];
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
    std::span<uint8_t> prepared,
    XgFieldProjectionCoverage* coverage) noexcept {
    XgObservedCompactFields extracted;
    const auto status = decode_xg_observed_fields(compact, extracted);
    if (status != XgFieldProjectionStatus::ok)
        return status;
    if (prepared.size() < kXgPreparedObservedOutputExtent)
        return XgFieldProjectionStatus::truncated_output;
    if (spans_overlap(compact, prepared))
        return XgFieldProjectionStatus::overlapping_buffers;

    // The helper's category 0 and 3 branches both rejoin these stores.
    // This still omits all conditional size, format, tile and swizzle rules.
    // The category-dependent token has no established semantic meaning.
    const uint32_t observed_token = extracted.category_06 == 0 ? 2u :
                                    extracted.category_06 == 3 ? 4u : 3u;
    // All three category branches rejoin explicit 8-byte zero stores.
    // In the ordinary category path, an index of 0 or >17 skips the
    // unverified shift lookup and leaves the shift count at zero.
    const bool known_shift = extracted.category_06 == 0 ||
                             extracted.category_06 == 3 ||
                             compact[0x09] == 0 || compact[0x09] > 17;
    write_le(prepared, 0x00, observed_token, 4);
    write_le(prepared, 0x08, 0, 8);
    write_le(prepared, 0x10, extracted.source_00, 8);
    write_le(prepared, 0x18,
             extracted.category_06 == 0 ? 1u : extracted.source_02, 4);
    write_le(prepared, 0x1c, extracted.source_04, 2);
    write_le(prepared, 0x1e, extracted.source_0a, 2);
    write_le(prepared, 0x20, extracted.source_07, 4);
    if (known_shift) write_le(prepared, 0x24, 1, 4);
    write_le(prepared, 0x28, 0, 8);
    if (coverage) *coverage = {known_shift, known_shift ? 44u : 40u};
    return XgFieldProjectionStatus::ok;
}
} // namespace scarlett::analysis
