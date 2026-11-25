#include "xg_vcall_arguments.h"
#include <array>
#include <cstdint>
#include <vector>

namespace scarlett::analysis {
bool verify_xg_dispatch_argument_setup_fixtures() noexcept {
    XgCallContext ctx;
    ctx.symbol = "XGCreateTextureComputer";
    ctx.call_rva = 0x3000;
    ctx.call_bytes[0] = 0xe8;

    // Entirely synthesized bytes, independent of the title:
    // MOV RCX,[RSP+0x20]; LEA RDX,[RSP+0x58];
    // MOV RAX,[RCX]; CALL [RAX+0x10].
    ctx.after = {
        0x48,0x8b,0x4c,0x24,0x20,
        0x48,0x8d,0x54,0x24,0x58,
        0x48,0x8b,0x01,
        0xff,0x50,0x10
    };
    std::vector<XgIndirectCallCandidate> calls;
    std::vector<XgArgSetupCandidate> args;
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        calls.size() != 1 ||
        !scan_xg_dispatch_argument_setups(
            std::span<const XgCallContext>(&ctx,1), calls, args) ||
        args.size() != 2 ||
        args[0].form != XgArgSetupForm::mov_rcx_from_memory ||
        args[0].setup_rva != 0x3005 ||
        args[0].memory_base_register != 4 ||
        args[0].displacement != 0x20 ||
        args[0].bytes_between_setup_and_call != 8 ||
        args[1].form != XgArgSetupForm::lea_rdx_from_rsp ||
        args[1].setup_rva != 0x300a ||
        args[1].memory_base_register != 4 ||
        args[1].displacement != 0x58 ||
        args[1].bytes_between_setup_and_call != 3)
        return false;

    // 32-bit positive displacement and extended register source.
    ctx.after = {
        0x49,0x8b,0x8e,0x80,0x00,0x00,0x00, // MOV RCX,[R14+128]
        0x48,0x8d,0x94,0x24,0x00,0x01,0x00,0x00, // LEA RDX,[RSP+256]
        0xff,0x50,0x08
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        calls.size() != 1 ||
        !scan_xg_dispatch_argument_setups(
            std::span<const XgCallContext>(&ctx,1), calls, args) ||
        args.size() != 2 ||
        args[0].memory_base_register != 14 ||
        args[0].displacement != 128 ||
        args[1].displacement != 256)
        return false;

    // Deliberately unsupported: indexed SIB, negative disp8,
    // LEA from a non-stack register, RIP-relative or register source.
    ctx.after = {
        0x48,0x8b,0x4c,0x4c,0x20, // indexed SIB -> reject
        0x48,0x8d,0x54,0x24,0xff, // negative RSP displacement -> reject
        0x48,0x8d,0x51,0x08,      // LEA RDX,[RCX+8] -> reject
        0x48,0x8b,0xc9,           // MOV RCX,RCX -> reject
        0xff,0x50,0x08
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        calls.size() != 1 ||
        !scan_xg_dispatch_argument_setups(
            std::span<const XgCallContext>(&ctx,1), calls, args) ||
        !args.empty())
        return false;
    // Entirely generated fallback: no signature in last 24 bytes.
    ctx.after = {0x48,0x8d,0x54,0x24,0x20};
    ctx.after.resize(40, 0x90);
    ctx.after.insert(ctx.after.end(), {0xff,0x50,0x08});
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        calls.size() != 1 ||
        !scan_xg_dispatch_argument_setups(
            std::span<const XgCallContext>(&ctx,1), calls, args) ||
        !args.empty())
        return false;

    // An intervening indirect call invalidates any older setup candidate:
    // only the fresh RCX setup after that call may belong to the next one.
    ctx.after = {
        0x48,0x8d,0x54,0x24,0x20, // LEA RDX,[RSP+0x20]
        0x48,0x8b,0x01,           // MOV RAX,[RCX]
        0xff,0x50,0x10,           // CALL [RAX+0x10]
        0x48,0x8b,0x4c,0x24,0x28, // MOV RCX,[RSP+0x28]
        0x48,0x8b,0x01,           // MOV RAX,[RCX]
        0xff,0x50,0x08            // CALL [RAX+0x08]
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        calls.size() != 2 ||
        !scan_xg_dispatch_argument_setups(
            std::span<const XgCallContext>(&ctx,1), calls, args) ||
        args.size() != 2 ||
        args[0].form != XgArgSetupForm::lea_rdx_from_rsp ||
        args[1].form != XgArgSetupForm::mov_rcx_from_memory ||
        args[1].displacement != 0x28)
        return false;

    // Malformed candidate metadata rejected atomically.
    ctx.after = {0x48,0x8b,0x4c,0x24,0x20,0xff,0x50,0x08};
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls))
        return false;
    args = {XgArgSetupCandidate{0x1234}};
    auto corrupted = calls;
    corrupted[0].displacement = 0x10;
    if (scan_xg_dispatch_argument_setups(
            std::span<const XgCallContext>(&ctx,1), corrupted, args) ||
        args.size() != 1 || args[0].source_call_rva != 0x1234)
        return false;
    corrupted = calls;
    corrupted[0].candidate_rva += 1;
    if (scan_xg_dispatch_argument_setups(
            std::span<const XgCallContext>(&ctx,1), corrupted, args) ||
        args.size() != 1 || args[0].source_call_rva != 0x1234)
        return false;

    // Duplicates suppressed; malformed oversized windows rejected.
    std::array<XgCallContext,2> repeated{ctx,ctx};
    if (!scan_xg_dispatch_argument_setups(repeated, calls, args) ||
        args.size() != 1 || args[0].displacement != 0x20)
        return false;
    repeated[1].after.resize(4097);
    if (scan_xg_dispatch_argument_setups(repeated, calls, args) ||
        args.size() != 1 || args[0].displacement != 0x20)
        return false;
    return true;
}
} // namespace scarlett::analysis
