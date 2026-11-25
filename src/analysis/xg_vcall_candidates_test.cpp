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
