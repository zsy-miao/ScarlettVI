#include "xg_vcall_loads.h"
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace scarlett::analysis {
bool verify_xg_table_load_fixtures() noexcept {
    XgCallContext ctx;
    ctx.symbol = "XGCreateTextureComputer";
    ctx.call_rva = 0x2000;
    ctx.call_bytes[0] = 0xe8;
    // Independently synthesized patterns; no game instruction bytes:
    //   mov r10,[rcx]; nop; call [r10+0x50];
    //   mov rax,[rcx]; call [rax+8].
    ctx.after = {0x4c,0x8b,0x11,0x90,0x41,0xff,0x52,0x50,
                 0x48,0x8b,0x01,0xff,0x50,0x08};
    std::vector<XgIndirectCallCandidate> calls;
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), calls) ||
        calls.size() != 2)
        return false;
    std::vector<XgTableLoadCandidate> produced;
    if (!scan_xg_table_load_candidates(
            std::span<const XgCallContext>(&ctx, 1), calls, produced) ||
        produced.size() != 2 ||
        produced[0].load_rva != 0x2005 ||
        produced[0].indirect_call_rva != 0x2009 ||
        produced[0].table_register != 10 ||
        produced[0].source_pointer_register != 1 ||
        produced[0].indirect_displacement != 0x50 ||
        produced[0].distance_to_call_bytes != 1 ||
        produced[1].load_rva != 0x200d ||
        produced[1].indirect_call_rva != 0x2010 ||
        produced[1].table_register != 0 ||
        produced[1].source_pointer_register != 1 ||
        produced[1].indirect_displacement != 8 ||
        produced[1].distance_to_call_bytes != 0)
        return false;

    // Unknown or unsupported producer pattern: no invented evidence.
    ctx.after[9] = 0x89; // no longer a MOV reg, [mem]
    if (!scan_xg_table_load_candidates(
            std::span<const XgCallContext>(&ctx, 1), calls, produced) ||
        produced.size() != 1)
        return false;
    ctx.after[9] = 0x8b;

    // A foreign candidate is invalid and cannot overwrite prior results.
    auto foreign = calls;
    foreign[0].source_call_rva = 0x3100;
    if (scan_xg_table_load_candidates(
            std::span<const XgCallContext>(&ctx, 1), foreign, produced) ||
        produced.size() != 1)
        return false;
    foreign = calls;
    foreign[0].candidate_rva = 0x1000;
    if (scan_xg_table_load_candidates(
            std::span<const XgCallContext>(&ctx, 1), foreign, produced) ||
        produced.size() != 1)
        return false;
    foreign = calls;
    foreign[0].base_register = 16;
    if (scan_xg_table_load_candidates(
            std::span<const XgCallContext>(&ctx, 1), foreign, produced) ||
        produced.size() != 1)
        return false;
    foreign = calls;
    foreign[0].displacement = 0x58; // claims wrong encoded displacement
    if (scan_xg_table_load_candidates(
            std::span<const XgCallContext>(&ctx, 1), foreign, produced) ||
        produced.size() != 1)
        return false;
    foreign = calls;
    foreign[0].instruction_bytes = 3; // ignores its REX prefix
    if (scan_xg_table_load_candidates(
            std::span<const XgCallContext>(&ctx, 1), foreign, produced) ||
        produced.size() != 1)
        return false;
    foreign = calls;
    foreign[0].candidate_rva += 1; // no FF /2 at this address
    if (scan_xg_table_load_candidates(
            std::span<const XgCallContext>(&ctx, 1), foreign, produced) ||
        produced.size() != 1)
        return false;

    // Duplicate verified windows must not duplicate the output.
    const std::array<XgCallContext, 2> repeat{{ctx, ctx}};
    if (!scan_xg_table_load_candidates(repeat, calls, produced) ||
        produced.size() != 2)
        return false;

    // REX.B source register extension is independently decoded.
    XgCallContext extended = ctx;
    extended.after = {0x49,0x8b,0x01,0xff,0x50,0x08};
    std::vector<XgIndirectCallCandidate> extended_calls;
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&extended, 1),
            extended_calls) || extended_calls.size() != 1 ||
        !scan_xg_table_load_candidates(
            std::span<const XgCallContext>(&extended, 1),
            extended_calls, produced) || produced.size() != 1 ||
        produced[0].source_pointer_register != 9 ||
        produced[0].table_register != 0)
        return false;

    // The lookback must not follow a tempting, but too-distant, load.
    extended.after = {0x48,0x8b,0x01};
    extended.after.resize(40, 0x90);
    extended.after.insert(extended.after.end(), {0xff,0x50,0x08});
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&extended, 1),
            extended_calls) || extended_calls.size() != 1 ||
        !scan_xg_table_load_candidates(
            std::span<const XgCallContext>(&extended, 1),
            extended_calls, produced) || !produced.empty())
        return false;

    // Overlarge input is rejected atomically, preserving a prior result.
    produced = {XgTableLoadCandidate{0x1234}};
    ctx.after.resize(4097);
    if (scan_xg_table_load_candidates(
            std::span<const XgCallContext>(&ctx, 1), calls, produced) ||
        produced.size() != 1 || produced[0].source_call_rva != 0x1234)
        return false;
    return true;
}
} // namespace scarlett::analysis
