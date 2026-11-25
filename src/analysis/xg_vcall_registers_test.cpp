#include "xg_vcall_registers.h"
#include <array>
#include <cstdint>
#include <vector>

namespace scarlett::analysis {
bool verify_xg_register_pattern_fixtures() noexcept {
    XgCallContext ctx;
    ctx.symbol = "XGCreateTextureComputer";
    ctx.call_rva = 0x3000;
    ctx.call_bytes[0] = 0xe8;
    // Generated synthetic data: offsets intentionally differ from the
    // user-owned title. Exercise chronological use of R8 before/after copy.
    ctx.after = {
        0x48,0x8d,0x54,0x24,0x40, // LEA RDX,[RSP+0x40]
        0x4c,0x8d,0x4c,0x24,0x50, // LEA R9,[RSP+0x50]
        0x4c,0x8d,0x74,0x24,0x60, // LEA R14,[RSP+0x60]
        0x4c,0x8d,0x44,0x24,0x70, // LEA R8,[RSP+0x70]
        0x48,0x8d,0x44,0x24,0x78, // LEA RAX,[RSP+0x78]
        0x4c,0x89,0x44,0x24,0x30, // MOV [RSP+0x30],R8
        0x4d,0x8b,0xc6,           // MOV R8,R14
        0x48,0x89,0x44,0x24,0x20, // MOV [RSP+0x20],RAX
        0x41,0xff,0x52,0x50       // CALL [R10+0x50]
    };
    std::vector<XgIndirectCallCandidate> calls;
    std::vector<XgRegisterPattern> timeline;
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), calls) ||
        calls.size() != 1 ||
        calls[0].candidate_rva != 0x302b ||
        !scan_xg_local_register_patterns(
            std::span<const XgCallContext>(&ctx, 1), calls, timeline) ||
        timeline.size() != 6)
        return false;
    const uint32_t call_rva = calls[0].candidate_rva;
    uint32_t result = 0xdeadbeefu;
    const auto resolve = [&](uint8_t reg, uint32_t at, uint32_t expected) {
        return resolve_xg_stack_address_pattern(
            timeline, ctx.call_rva, call_rva, reg, at, result) &&
            result == expected;
    };
    if (!resolve(2, call_rva, 0x40) || // RDX
        !resolve(9, call_rva, 0x50) || // R9
        !resolve(8, call_rva, 0x60) || // R8 copied from R14
        !resolve(8, 0x301e, 0x70) || // R8 before copy: store use
        !resolve(0, 0x3026, 0x78))  // RAX: store use
        return false;
    if (resolve_xg_stack_address_pattern(
            timeline, ctx.call_rva, call_rva, 1, call_rva, result) ||
        result != 0x78)
        return false; // unknown RCX leaves prior result unchanged
    if (resolve_xg_stack_address_pattern(
            timeline, ctx.call_rva, call_rva, 16, call_rva, result) ||
        result != 0x78)
        return false;

    // Register-copy opcode 89 (destination in ModRM.r/m), plus disp32.
    ctx.after = {
        0x4c,0x8d,0xb4,0x24,0x80,0x00,0x00,0x00, // R14=[RSP+0x80]
        0x4d,0x89,0xf0,                           // R8 <- R14 via 89
        0x41,0xff,0x52,0x50
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), calls) ||
        !scan_xg_local_register_patterns(
            std::span<const XgCallContext>(&ctx, 1), calls, timeline) ||
        timeline.size() != 2 ||
        !resolve_xg_stack_address_pattern(
            timeline, ctx.call_rva, calls[0].candidate_rva,
            8, calls[0].candidate_rva, result) ||
        result != 0x80)
        return false;

    // Previous indirect dispatch acts as a register-clobber barrier.
    ctx.after = {
        0x48,0x8d,0x54,0x24,0x40,
        0xff,0x50,0x10,
        0x41,0xff,0x52,0x50
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), calls) ||
        calls.size() != 2 ||
        !scan_xg_local_register_patterns(
            std::span<const XgCallContext>(&ctx, 1), calls, timeline) ||
        !timeline.empty())
        return false;

    // Invalid LEA forms: negative disp8, scaled-index SIB,
    // missing REX.W, negative/overlarge disp32, truncated disp32.
    ctx.after = {
        0x48,0x8d,0x54,0x24,0xff,
        0x48,0x8d,0x54,0x4c,0x40,
        0x8d,0x54,0x24,0x40,
        0x48,0x8d,0x94,0x24,0x01,0x10,0x00,0x00,
        0x48,0x8d,0x94,
        0x41,0xff,0x52,0x50
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), calls) ||
        !scan_xg_local_register_patterns(
            std::span<const XgCallContext>(&ctx, 1), calls, timeline) ||
        !timeline.empty())
        return false;

    // The same context/call twice must not duplicate observations.
    ctx.after = {
        0x48,0x8d,0x54,0x24,0x40,
        0x41,0xff,0x52,0x50
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), calls))
        return false;
    const std::array<XgCallContext,2> repeated{{ctx,ctx}};
    if (!scan_xg_local_register_patterns(repeated, calls, timeline) ||
        timeline.size() != 1)
        return false;

    // Wrong candidate metadata is rejected without mutating result.
    auto corrupt = calls;
    corrupt[0].displacement = 0x58;
    if (scan_xg_local_register_patterns(
            std::span<const XgCallContext>(&ctx, 1), corrupt, timeline) ||
        timeline.size() != 1)
        return false;
    ctx.after.resize(4097, 0x90);
    if (scan_xg_local_register_patterns(
            std::span<const XgCallContext>(&ctx, 1), calls, timeline) ||
        timeline.size() != 1)
        return false;

    // Outside the 96-byte lookback: no guessed producer.
    ctx.after = {0x48,0x8d,0x54,0x24,0x40};
    ctx.after.resize(105, 0x90);
    ctx.after.insert(ctx.after.end(),{0x41,0xff,0x52,0x50});
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx, 1), calls) ||
        !scan_xg_local_register_patterns(
            std::span<const XgCallContext>(&ctx, 1), calls, timeline) ||
        !timeline.empty())
        return false;

    // A synthetic self-cycle must not resolve or alter the output.
    XgRegisterPattern loop;
    loop.source_call_rva = 0x1000;
    loop.indirect_call_rva = 0x1020;
    loop.instruction_rva = 0x1010;
    loop.kind = XgRegPatternKind::register_copy;
    loop.destination_gpr = 8;
    loop.source_gpr = 8;
    result = 1234;
    if (resolve_xg_stack_address_pattern(
            std::span<const XgRegisterPattern>(&loop,1),
            0x1000,0x1020,8,0x1020,result) || result != 1234)
        return false;
    return true;
}
} // namespace scarlett::analysis
