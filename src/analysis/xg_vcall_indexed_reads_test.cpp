#include "xg_vcall_indexed_reads.h"
#include <array>
#include <cstdint>
#include <vector>

namespace scarlett::analysis {
bool verify_xg_postcall_indexed_read_fixtures() noexcept {
    XgCallContext ctx;
    ctx.symbol = "XGCreateTextureComputer";
    ctx.call_rva = 0x3000;
    ctx.call_bytes[0] = 0xe8;

    // Separately generated encodings with stack and record offsets
    // intentionally different from the inspected game executable:
    // LEA RDX,[RSP+0x30]; CALL [RAX+0x10];
    // MOV ECX,[RSP+RAX+0x8C]; MOV RCX,[RSP+RAX+0x90];
    // MOV RAX,[RSP+RAX+0x98]. No guest code is executed.
    ctx.after = {
        0x48,0x8d,0x54,0x24,0x30,
        0xff,0x50,0x10,
        0x8b,0x8c,0x04,0x8c,0x00,0x00,0x00,
        0x48,0x8b,0x8c,0x04,0x90,0x00,0x00,0x00,
        0x48,0x8b,0x84,0x04,0x98,0x00,0x00,0x00
    };
    std::vector<XgIndirectCallCandidate> calls;
    std::vector<XgPostcallIndexedRead> reads;
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        calls.size() != 1 ||
        !scan_xg_postcall_indexed_reads(
            std::span<const XgCallContext>(&ctx,1), calls, reads) ||
        reads.size() != 3 ||
        reads[0].read_rva != 0x300d ||
        reads[0].passed_rsp_offset != 0x30 ||
        reads[0].relative_displacement != 0x5c ||
        reads[0].destination_gpr != 1 ||
        reads[0].width_bytes != 4 ||
        reads[0].bytes_after_dispatch != 0 ||
        reads[1].read_rva != 0x3014 ||
        reads[1].relative_displacement != 0x60 ||
        reads[1].width_bytes != 8 ||
        reads[1].bytes_after_dispatch != 7 ||
        reads[2].read_rva != 0x301c ||
        reads[2].relative_displacement != 0x68 ||
        reads[2].destination_gpr != 0 ||
        reads[2].bytes_after_dispatch != 15 ||
        reads[0].copied_immediately_to_rsi ||
        reads[1].copied_immediately_to_rsi ||
        reads[2].copied_immediately_to_rsi)
        return false;

    // Synthetic adjacent same-width transfers to RSI fields, including
    // an explicit negative case for a mismatched source GPR.
    ctx.after = {
        0x48,0x8d,0x54,0x24,0x30,
        0xff,0x50,0x10,
        0x8b,0x8c,0x04,0x70,0x00,0x00,0x00,
        0x89,0x4e,0x14,
        0x48,0x8b,0x84,0x04,0x80,0x00,0x00,0x00,
        0x48,0x89,0x46,0x28
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        !scan_xg_postcall_indexed_reads(
            std::span<const XgCallContext>(&ctx,1), calls, reads) ||
        reads.size() != 2 ||
        !reads[0].copied_immediately_to_rsi ||
        reads[0].copy_rva != 0x3014 ||
        reads[0].rsi_field_offset != 0x14 ||
        !reads[1].copied_immediately_to_rsi ||
        reads[1].copy_rva != 0x301f ||
        reads[1].rsi_field_offset != 0x28)
        return false;
    ctx.after[16] = 0x46; // EAX source rather than loaded ECX
    if (!scan_xg_postcall_indexed_reads(
            std::span<const XgCallContext>(&ctx,1), calls, reads) ||
        reads.size() != 2 ||
        reads[0].copied_immediately_to_rsi ||
        !reads[1].copied_immediately_to_rsi)
        return false;
    ctx.after[16] = 0x4e;
    ctx.after.insert(ctx.after.begin() + 15, 0x90); // extra instruction
    if (!scan_xg_postcall_indexed_reads(
            std::span<const XgCallContext>(&ctx,1), calls, reads) ||
        reads.size() != 2 ||
        reads[0].copied_immediately_to_rsi)
        return false;

    // Extended destination register via REX.R; scale remains 1 and
    // RSP and RAX remain actual base/index.
    ctx.after = {
        0x48,0x8d,0x54,0x24,0x30,
        0xff,0x50,0x10,
        0x4c,0x8b,0x8c,0x04,0x70,0x00,0x00,0x00,
        0x4c,0x89,0x4e,0x20
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        !scan_xg_postcall_indexed_reads(
            std::span<const XgCallContext>(&ctx,1), calls, reads) ||
        reads.size() != 1 ||
        reads[0].destination_gpr != 9 ||
        reads[0].width_bytes != 8 ||
        reads[0].relative_displacement != 0x40 ||
        !reads[0].copied_immediately_to_rsi ||
        reads[0].rsi_field_offset != 0x20)
        return false;

    // Deliberately unsupported: scaled RAX index, alternate base, a
    // signed negative disp8, a nonstack RDX setup, and too-large offsets.
    ctx.after = {
        0x48,0x8d,0x54,0x24,0x30,
        0xff,0x50,0x10,
        0x8b,0x8c,0x44,0x70,0x00,0x00,0x00,
        0x48,0x8b,0x8c,0x05,0x70,0x00,0x00,0x00,
        0x8b,0x4c,0x04,0xf8,
        0x8b,0x8c,0x04,0xa8,0x00,0x00,0x00
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        !scan_xg_postcall_indexed_reads(
            std::span<const XgCallContext>(&ctx,1), calls, reads) ||
        !reads.empty())
        return false;
    ctx.after = {
        0x48,0x8d,0x54,0x24,0x30,
        0xff,0x50,0x10,
        0x8b,0x8c,0x04,0x70,0x00,0x00,0x00
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        !scan_xg_postcall_indexed_reads(
            std::span<const XgCallContext>(&ctx,1), calls, reads) ||
        reads.size() != 1)
        return false;
    ctx.after[3] = 0x01; // LEA RDX,[RCX+...] unsupported by scanner
    if (!scan_xg_postcall_indexed_reads(
            std::span<const XgCallContext>(&ctx,1), calls, reads) ||
        !reads.empty())
        return false;

    // Respect a subsequent verified indirect call as a scan barrier.
    ctx.after = {
        0x48,0x8d,0x54,0x24,0x30,
        0xff,0x50,0x10,
        0xff,0x50,0x08,
        0x8b,0x8c,0x04,0x70,0x00,0x00,0x00
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        calls.size() != 2 ||
        !scan_xg_postcall_indexed_reads(
            std::span<const XgCallContext>(&ctx,1), calls, reads) ||
        !reads.empty())
        return false;

    // Missing LEA RDX setup => unknown base, never invent a correlation.
    ctx.after = {
        0x90,0x90,0x90,0x90,
        0xff,0x50,0x10,
        0x8b,0x8c,0x04,0x70,0x00,0x00,0x00
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        !scan_xg_postcall_indexed_reads(
            std::span<const XgCallContext>(&ctx,1), calls, reads) ||
        !reads.empty())
        return false;

    // A read after the 96-byte horizon is not reported.
    ctx.after = {
        0x48,0x8d,0x54,0x24,0x30,
        0xff,0x50,0x10
    };
    ctx.after.resize(106, 0x90);
    ctx.after.insert(ctx.after.end(),
        {0x8b,0x8c,0x04,0x70,0x00,0x00,0x00});
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        !scan_xg_postcall_indexed_reads(
            std::span<const XgCallContext>(&ctx,1), calls, reads) ||
        !reads.empty())
        return false;

    // Duplicate contexts cannot duplicate output; invalid metadata,
    // truncated context and overlarge contexts are rejected atomically.
    ctx.after = {
        0x48,0x8d,0x54,0x24,0x30,
        0xff,0x50,0x10,
        0x8b,0x8c,0x04,0x70,0x00,0x00,0x00
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls))
        return false;
    const std::array<XgCallContext,2> repeated{{ctx,ctx}};
    if (!scan_xg_postcall_indexed_reads(repeated, calls, reads) ||
        reads.size() != 1 || reads[0].relative_displacement != 0x40)
        return false;
    const auto old_read = reads[0];
    auto invalid = calls;
    invalid[0].displacement = 0x18;
    if (scan_xg_postcall_indexed_reads(
            std::span<const XgCallContext>(&ctx,1), invalid, reads) ||
        reads.size() != 1 ||
        reads[0].read_rva != old_read.read_rva)
        return false;
    ctx.after.resize(4097,0x90);
    if (scan_xg_postcall_indexed_reads(
            std::span<const XgCallContext>(&ctx,1), calls, reads) ||
        reads.size() != 1)
        return false;
    ctx.after.resize(5);
    if (scan_xg_postcall_indexed_reads(
            std::span<const XgCallContext>(&ctx,1), calls, reads) ||
        reads.size() != 1)
        return false;
    return true;
}
} // namespace scarlett::analysis
