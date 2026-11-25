#include "xg_vcall_candidates.h"
#include <array>
#include <cstdint>
#include <vector>

namespace scarlett::analysis {
bool verify_xg_indirect_postcall_fixtures() noexcept {
    XgCallContext ctx;
    ctx.symbol = "XGCreateTextureComputer";
    ctx.call_rva = 0x1000;
    ctx.call_bytes[0] = 0xe8;
    // Generated x64 byte patterns only, not title code:
    // call [rax+0x10], call [r10+0x50], call [rax+0x108],
    // negative disp, unaligned disp, indexed call, truncated disp32.
    ctx.after = {
        0x90,
        0xff, 0x50, 0x10,
        0x90,
        0x41, 0xff, 0x52, 0x50,
        0x90,
        0xff, 0x90, 0x08, 0x01, 0x00, 0x00,
        0x90,
        0xff, 0x50, 0xf8,
        0x90,
        0xff, 0x50, 0x03,
        0x90,
        0xff, 0x54, 0x24, 0x08,
        0x90,
        0xff, 0x90, 0x20
    };
    std::vector<XgIndirectCallCandidate> matches;
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), matches) ||
        matches.size() != 3 ||
        matches[0].candidate_rva != 0x1006 ||
        matches[0].base_register != 0 ||
        matches[0].displacement != 0x10 ||
        matches[0].instruction_bytes != 3 ||
        matches[1].candidate_rva != 0x100a ||
        matches[1].base_register != 10 ||
        matches[1].displacement != 0x50 ||
        matches[1].instruction_bytes != 4 ||
        matches[2].candidate_rva != 0x100f ||
        matches[2].base_register != 0 ||
        matches[2].displacement != 0x108 ||
        matches[2].instruction_bytes != 6)
        return false;

    const auto baseline = ctx.after;

    // A generated preceding LEA has displacement 0x40. Its last byte
    // must NOT be mistaken for a redundant REX prefix on the FF call.
    // Only the actual FF opcode should be a candidate start.
    ctx.after = {
        0x48, 0x8d, 0x54, 0x24, 0x40, // LEA RDX,[RSP+0x40]
        0xff, 0x50, 0x10             // CALL [RAX+0x10]
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), matches) ||
        matches.size() != 1 ||
        matches[0].candidate_rva != 0x100a ||
        matches[0].instruction_bytes != 3 ||
        matches[0].base_register != 0 ||
        matches[0].displacement != 0x10)
        return false;
    // The same ambiguity occurs for a trailing 0x48 (REX.W).
    ctx.after[4] = 0x48;
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), matches) ||
        matches.size() != 1 ||
        matches[0].candidate_rva != 0x100a ||
        matches[0].instruction_bytes != 3)
        return false;

    // A preceding complete REX.W LEA can also end in a byte 0x41 or
    // 0x49 that *would* otherwise look like a necessary REX.B prefix.
    // Prefer the unprefixed FF at the next byte for these recognized
    // local overlaps; independently confirm instruction boundaries.
    for (const uint8_t displacement : {uint8_t{0x41},
                                       uint8_t{0x49}}) {
        ctx.after = {
            0x4c, 0x8d, 0x4c, 0x24, displacement,
            0xff, 0x50, 0x10
        };
        if (!scan_xg_indirect_postcalls(
                std::span<const XgCallContext>(&ctx, 1), matches) ||
            matches.size() != 1 ||
            matches[0].candidate_rva != 0x100a ||
            matches[0].instruction_bytes != 3 ||
            matches[0].base_register != 0 ||
            matches[0].displacement != 0x10)
            return false;
    }

    // A real REX.B bit is necessary to express an extended base GPR.
    // Both 0x41 (B) and 0x49 (W+B) retain their original call offset.
    for (const uint8_t prefix : {uint8_t{0x41}, uint8_t{0x49}}) {
        ctx.after = {prefix, 0xff, 0x52, 0x50};
        if (!scan_xg_indirect_postcalls(
                std::span<const XgCallContext>(&ctx, 1), matches) ||
            matches.size() != 1 ||
            matches[0].candidate_rva != 0x1005 ||
            matches[0].base_register != 10 ||
            matches[0].instruction_bytes != 4 ||
            matches[0].displacement != 0x50)
            return false;
    }

    // A redundant X-only prefix is not used as a reliable start
    // (though the later unprefixed FF sequence remains a raw match).
    ctx.after = {0x42, 0xff, 0x50, 0x10};
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), matches) ||
        matches.size() != 1 ||
        matches[0].candidate_rva != 0x1006 ||
        matches[0].base_register != 0 ||
        matches[0].instruction_bytes != 3)
        return false;

    // Resume the original generated corpus and atomic-failure checks.
    ctx.after = baseline;
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), matches) ||
        matches.size() != 3)
        return false;

    // Synthetic duplicate sections should not double the report.
    const std::array<XgCallContext, 2> duplicates{{ctx, ctx}};
    if (!scan_xg_indirect_postcalls(duplicates, matches) ||
        matches.size() != 3)
        return false;

    // Failure is atomic and never overwrites previous results.
    ctx.call_rva = UINT32_MAX - 2;
    if (scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), matches) ||
        matches.size() != 3)
        return false;
    ctx.call_rva = 0x1000;
    ctx.call_bytes[0] = 0x90;
    if (scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), matches) ||
        matches.size() != 3)
        return false;
    ctx.call_bytes[0] = 0xe8;
    ctx.after.resize(4097, 0x90);
    if (scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), matches) ||
        matches.size() != 3)
        return false;
    return true;
}
} // namespace scarlett::analysis
