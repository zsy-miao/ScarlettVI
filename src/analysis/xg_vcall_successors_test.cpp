#include "xg_vcall_successors.h"
#include <array>
#include <cstdint>
#include <vector>

namespace scarlett::analysis {
bool verify_xg_dispatch_successor_fixtures() noexcept {
    XgCallContext ctx;
    ctx.symbol = "XGCreateTextureComputer";
    ctx.call_rva = 0x1000;
    ctx.call_bytes[0] = 0xe8;
    // All generated synthetic patterns, no game bytes:
    //  MOV RCX,[R15]; NOP; CALL [R10+0x50];
    //  MOV RCX,[R15]; MOV RAX,[RCX]; CALL [RAX+0x08].
    ctx.after = {
        0x49,0x8b,0x0f, 0x90,
        0x41,0xff,0x52,0x50,
        0x49,0x8b,0x0f,
        0x48,0x8b,0x01,
        0xff,0x50,0x08
    };
    std::vector<XgIndirectCallCandidate> calls;
    std::vector<XgDispatchSuccessorEvidence> found;
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        calls.size() != 2 ||
        !scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), calls, found) ||
        found.size() != 1 ||
        found[0].first_indirect_rva != 0x1009 ||
        found[0].next_indirect_rva != 0x1013 ||
        found[0].first_dispatch_offset != 0x50 ||
        found[0].reload_rcx_rva != 0x100d ||
        found[0].reload_table_rva != 0x1010 ||
        found[0].reload_base_gpr != 15 ||
        found[0].reload_displacement != 0 ||
        !found[0].prior_matching_storage_pattern ||
        found[0].prior_load_rva != 0x1005)
        return false;

    // Generated second motif: two calls with same RSP storage slot. A
    // +0x10 followed immediately by a +0x08, not necessarily COM methods.
    ctx.after = {
        0x48,0x8b,0x4c,0x24,0x20,
        0xff,0x50,0x10,
        0x48,0x8b,0x4c,0x24,0x20,
        0x48,0x8b,0x01,
        0xff,0x50,0x08
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        calls.size() != 2 ||
        !scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), calls, found) ||
        found.size() != 1 ||
        found[0].first_indirect_rva != 0x100a ||
        found[0].next_indirect_rva != 0x1015 ||
        found[0].reload_base_gpr != 4 ||
        found[0].reload_displacement != 0x20 ||
        !found[0].prior_matching_storage_pattern)
        return false;

    // The same instruction forms, but the storage offsets differ:
    // preserve an observed motif WITHOUT claiming identical storage.
    ctx.after[4] = 0x28;
    if (!scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), calls, found) ||
        found.size() != 1 ||
        found[0].prior_matching_storage_pattern)
        return false;

    // Positive disp32 on an extended GPR base through a no-index SIB.
    // MOV RCX,[R12+0x100], CALL +0x10, same MOV, table load, CALL +0x08.
    ctx.after = {
        0x49,0x8b,0x8c,0x24,0x00,0x01,0x00,0x00,
        0xff,0x50,0x10,
        0x49,0x8b,0x8c,0x24,0x00,0x01,0x00,0x00,
        0x48,0x8b,0x01,
        0xff,0x50,0x08
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        !scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), calls, found) ||
        found.size() != 1 || found[0].reload_base_gpr != 12 ||
        found[0].reload_displacement != 0x100 ||
        !found[0].prior_matching_storage_pattern)
        return false;
    ctx.after[16] = 0x11; // outgoing +0x1100 exceeds bound 4096
    if (!scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), calls, found) ||
        !found.empty())
        return false;

    ctx.after = {
        0x48,0x8b,0x4c,0x24,0x20,
        0xff,0x50,0x10,
        0x48,0x8b,0x4c,0x24,0x20,
        0x48,0x8b,0x01,
        0xff,0x50,0x08
    };
    // No false positive when an intervening NOP breaks the *exact*
    // object+table reload sequence.
    ctx.after.insert(ctx.after.begin() + 13, 0x90);
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        !scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), calls, found) ||
        !found.empty())
        return false;

    // Unsupported indexed SIB addressing and negative disp8 rejected.
    ctx.after = {
        0xff,0x50,0x10,
        0x48,0x8b,0x4c,0x4c,0x20,
        0x48,0x8b,0x01,
        0xff,0x50,0x08
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        !scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), calls, found) ||
        !found.empty())
        return false;
    ctx.after[6] = 0x24;
    ctx.after[7] = 0xff;
    if (!scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), calls, found) ||
        !found.empty())
        return false;

    // No successor over the 16-byte inter-call gap.
    ctx.after = {0xff,0x50,0x10};
    ctx.after.resize(24, 0x90);
    ctx.after.insert(ctx.after.end(),
        {0x48,0x8b,0x4c,0x24,0x20,0x48,0x8b,0x01,0xff,0x50,0x08});
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        !scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), calls, found) ||
        !found.empty())
        return false;

    // Real matching structure again for metadata-negative, dedup and
    // atomic output tests. No title bytes included.
    ctx.after = {
        0xff,0x50,0x10,
        0x48,0x8b,0x4c,0x24,0x20,
        0x48,0x8b,0x01,
        0xff,0x50,0x08
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        !scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), calls, found) ||
        found.size() != 1 ||
        found[0].prior_matching_storage_pattern)
        return false;
    const auto before = found[0];
    auto invalid = calls;
    invalid[1].candidate_rva += 1;
    if (scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), invalid, found) ||
        found.size() != 1 ||
        found[0].next_indirect_rva != before.next_indirect_rva)
        return false;
    invalid = calls;
    invalid[0].displacement = 0x18;
    if (scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), invalid, found) ||
        found.size() != 1)
        return false;

    const std::array<XgCallContext,2> repeated{{ctx,ctx}};
    if (!scan_xg_dispatch_successors(repeated, calls, found) ||
        found.size() != 1)
        return false;
    ctx.after.resize(4097);
    if (scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), calls, found) ||
        found.size() != 1)
        return false;
    ctx.after = {0xff,0x50,0x10,
                 0x48,0x8b,0x4c,0x24,0x20,
                 0x48,0x8b,0x01,
                 0xff,0x50,0x08};
    ctx.call_bytes[0] = 0x90;
    if (scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), calls, found) ||
        found.size() != 1)
        return false;

    // The scanner cannot invent successor motifs from truncated code.
    ctx.call_bytes[0] = 0xe8;
    ctx.after.resize(8);
    if (scan_xg_dispatch_successors(
            std::span<const XgCallContext>(&ctx,1), calls, found) ||
        found.size() != 1)
        return false;
    return true;
}
} // namespace scarlett::analysis
