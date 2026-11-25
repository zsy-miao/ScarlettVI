#include "xg_field_projection.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace scarlett::analysis {
namespace {
using Status = XgFieldProjectionStatus;

uint32_t next_random(uint32_t& state) noexcept {
    // Deterministic generated-data only; no external files or entropy.
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void reference_store(std::span<uint8_t> out, size_t offset,
                     uint16_t source_word, size_t width) {
    out[offset] = static_cast<uint8_t>(source_word);
    out[offset + 1] = static_cast<uint8_t>(source_word >> 8);
    for (size_t i = 2; i < width; ++i)
        out[offset + i] = 0;
}
} // namespace

bool verify_xg_field_projection_fixtures() noexcept {
    std::array<uint8_t, 14> compact{};
    std::array<uint8_t, 64> projected{};
    std::array<uint8_t, 64> expected{};
    uint32_t rng = 0x7182a63cu;
    for (size_t trial = 0; trial < 512; ++trial) {
        for (auto& value : compact)
            value = static_cast<uint8_t>(next_random(rng));
        for (auto& value : projected)
            value = static_cast<uint8_t>(next_random(rng));
        expected = projected;
        // Exercise both alternate entries and the unknown lookup interval.
        compact[9] = trial % 7 == 0 ? uint8_t{0} :
                     trial % 7 == 1 ? uint8_t{17} :
                     trial % 7 == 2 ? uint8_t{18} : compact[9];
        // Exercise both alternative entry paths and the shared default.
        compact[6] = trial % 9 == 0 ? uint8_t{0} :
                     trial % 9 == 1 ? uint8_t{3} :
                     static_cast<uint8_t>(1 + (trial % 254));

        const uint16_t w0 = static_cast<uint16_t>(
            unsigned(compact[0]) | unsigned(compact[1]) << 8);
        const uint16_t w2 = static_cast<uint16_t>(
            unsigned(compact[2]) | unsigned(compact[3]) << 8);
        const uint16_t w4 = static_cast<uint16_t>(
            unsigned(compact[4]) | unsigned(compact[5]) << 8);

        // Independently specified expected destinations, including zero
        // extension and preservation of all unobserved bytes.
        expected[0] = compact[6] == 0 ? 2 : compact[6] == 3 ? 4 : 3;
        expected[1] = expected[2] = expected[3] = 0;
        for (size_t i = 8; i < 16; ++i) expected[i] = 0;
        reference_store(expected, 0x10, w0, 8);
        reference_store(expected, 0x18,
                        compact[6] == 0 ? uint16_t{1} : w2, 4);
        reference_store(expected, 0x1c, w4, 2);
        reference_store(expected, 0x1e, compact[0x0a], 2);
        reference_store(expected, 0x20, compact[0x07], 4);
        const bool lookup_known = compact[6] == 0 || compact[6] == 3 ||
                                  compact[9] == 0 || compact[9] > 17;
        if (lookup_known) reference_store(expected, 0x24, 1, 4);
        for (size_t i = 0x28; i < 0x30; ++i) expected[i] = 0;

        XgObservedCompactFields extracted{};
        XgFieldProjectionCoverage coverage{false, 99};
        if (decode_xg_observed_fields(compact, extracted) != Status::ok ||
            extracted.category_06 != compact[6] ||
            extracted.source_00 != w0 ||
            extracted.source_02 != w2 ||
            extracted.source_04 != w4 ||
            extracted.source_07 != compact[7] ||
            extracted.source_0a != compact[10] ||
            project_xg_observed_fields(compact, projected, &coverage) != Status::ok ||
            coverage.output_24_known != lookup_known ||
            coverage.modeled_bytes != (lookup_known ? 44u : 40u) ||
            projected != expected)
            return false;
    }

    // Boundary lengths must fail without mutation.
    compact.fill(0xff);
    compact[6] = 1;
    projected.fill(0x4c);
    const auto pristine = projected;
    XgFieldProjectionCoverage unchanged_coverage{true, 987};
    XgObservedCompactFields untouched{0xa1, 0x1234, 0x5678, 0x9abc, 0xde, 0xf0};
    if (decode_xg_observed_fields(
            std::span<const uint8_t>(compact).first(10),
            untouched) != Status::truncated_input ||
        untouched.source_00 != 0x1234 ||
        untouched.source_0a != 0xf0 ||
        project_xg_observed_fields(
            std::span<const uint8_t>(compact).first(10),
            projected, &unchanged_coverage) != Status::truncated_input ||
        unchanged_coverage.modeled_bytes != 987 ||
        projected != pristine ||
        project_xg_observed_fields(
            compact, std::span<uint8_t>(projected).first(0x2f),
            &unchanged_coverage) != Status::truncated_output ||
        unchanged_coverage.modeled_bytes != 987 ||
        projected != pristine)
        return false;
    // The zero and 3 category entry paths are independently corroborated:
    // both reach the shared field stores but supply different token/word.
    for (const uint8_t branch : {uint8_t{0}, uint8_t{3}}) {
        compact[6] = branch;
        projected = pristine;
        if (decode_xg_observed_fields(compact, untouched) != Status::ok ||
            untouched.category_06 != branch ||
            project_xg_observed_fields(compact, projected) != Status::ok ||
            projected[0] != (branch == 0 ? 2 : 4) ||
            projected[0x18] != (branch == 0 ? 1 : 0xff) ||
            projected[0x19] != (branch == 0 ? 0 : 0xff))
            return false;
    }
    // Cover all lookup-index boundaries without claiming the unknown table.
    compact[6] = 1;
    for (unsigned index = 0; index < 256; ++index) {
        compact[9] = static_cast<uint8_t>(index);
        projected = pristine;
        XgFieldProjectionCoverage coverage{};
        const bool known = index == 0 || index > 17;
        if (project_xg_observed_fields(compact, projected, &coverage) !=
                Status::ok ||
            coverage.output_24_known != known ||
            coverage.modeled_bytes != (known ? 44u : 40u) ||
            (known && (projected[0x24] != 1 || projected[0x25] != 0 ||
                       projected[0x26] != 0 || projected[0x27] != 0)) ||
            (!known && (projected[0x24] != 0x4c ||
                        projected[0x27] != 0x4c)))
            return false;
    }
    compact[9] = 0;
    projected = pristine;

    // Reject overlapping input and output ranges even when the destination
    // fits, so a partial field projection cannot silently destroy its input.
    std::array<uint8_t, 80> shared{};
    shared.fill(0x7f);
    shared[6] = 1;
    const auto original_shared = shared;
    if (project_xg_observed_fields(
            std::span<const uint8_t>(shared).first(14),
            std::span<uint8_t>(shared).subspan(4, 64),
            &unchanged_coverage) != Status::overlapping_buffers ||
        unchanged_coverage.modeled_bytes != 987 ||
        shared != original_shared)
        return false;

    // Adjacent (nonoverlapping) spans are valid, even within one allocation.
    if (project_xg_observed_fields(
            std::span<const uint8_t>(shared).first(14),
            std::span<uint8_t>(shared).subspan(14, 64)) != Status::ok)
        return false;

    // Odd destination byte alignment must be valid (bytewise writes only).
    std::array<uint8_t, 65> shifted{};
    shifted.fill(0x5a);
    if (project_xg_observed_fields(compact,
            std::span<uint8_t>(shifted).subspan(1, 64)) != Status::ok ||
        shifted[0] != 0x5a || shifted[0x10 + 1] != 0xff ||
        shifted[0x10 + 2] != 0xff || shifted[0x10 + 3] != 0 ||
        shifted[64] != 0x5a)
        return false;
    return true;
}
} // namespace scarlett::analysis
