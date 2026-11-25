#include "xg_vcall_stack_writes.h"
#include <array>
#include <cstdint>
#include <vector>

namespace scarlett::analysis {
bool verify_xg_postcall_stack_write_fixtures() noexcept {
    XgCallContext ctx;
    ctx.symbol = "XGCreateTextureComputer";
    ctx.call_rva = 0x1000;
    ctx.call_bytes[0] = 0xe8;
    // Generated independent x64 test encodings only.
    // MOV [RSP+0x30], R8 (64 bit), MOV [RSP+0x28], ESI (32 bit),
    // MOV [RSP+0x20], RAX (64 bit), CALL [R10+0x50].
    ctx.after = {
        0x4c,0x89,0x44,0x24,0x30,
        0x89,0x74,0x24,0x28,
        0x48,0x89,0x44,0x24,0x20,
        0x41,0xff,0x52,0x50
    };
    std::vector<XgIndirectCallCandidate> calls;
    std::vector<XgStackWriteCandidate> slots;
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        calls.size() != 1 ||
        !scan_xg_postcall_stack_writes(
            std::span<const XgCallContext>(&ctx,1), calls, slots) ||
        slots.size() != 3 ||
        slots[0].stack_offset != 0x20 || slots[0].source_register != 0 ||
        slots[0].width_bytes != 8 || slots[0].gap_to_call_bytes != 0 ||
        slots[1].stack_offset != 0x28 || slots[1].source_register != 6 ||
        slots[1].width_bytes != 4 || slots[1].gap_to_call_bytes != 5 ||
        slots[2].stack_offset != 0x30 || slots[2].source_register != 8 ||
        slots[2].width_bytes != 8 || slots[2].gap_to_call_bytes != 9)
        return false;

    // Full disp32 and REX.R extension: [RSP+0x80] receives R8.
    ctx.after = {0x4c,0x89,0x84,0x24,0x80,0x00,0x00,0x00,
                 0xff,0x50,0x08};
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        !scan_xg_postcall_stack_writes(
            std::span<const XgCallContext>(&ctx,1), calls, slots) ||
        slots.size() != 1 || slots[0].stack_offset != 0x80 ||
        slots[0].source_register != 8 || slots[0].width_bytes != 8)
        return false;
    // A 32-bit displacement just beyond the bounded slot range is unknown.
    ctx.after[4] = 0x88;
    if (!scan_xg_postcall_stack_writes(
            std::span<const XgCallContext>(&ctx,1), calls, slots) ||
        !slots.empty())
        return false;

    // Two writes into the same outgoing slot: use the nearest one.
    ctx.after = {
        0x89,0x44,0x24,0x20,
        0x48,0x89,0x54,0x24,0x20,
        0xff,0x50,0x08
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        !scan_xg_postcall_stack_writes(
            std::span<const XgCallContext>(&ctx,1), calls, slots) ||
        slots.size() != 1 || slots[0].source_register != 2 ||
        slots[0].width_bytes != 8 || slots[0].gap_to_call_bytes != 0)
        return false;

    // Various unsupported offsets, negative displacements, indexed SIB,
    // wrong opcode or truncated stores are deliberately ignored.
    ctx.after = {
        0x48,0x89,0x44,0x24,0x18,
        0x48,0x89,0x44,0x24,0xf8,
        0x48,0x89,0x44,0x4c,0x20,
        0x48,0x8b,0x44,0x24,0x20,
        0x48,0x89,0x44,
        0xff,0x50,0x08
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        !scan_xg_postcall_stack_writes(
            std::span<const XgCallContext>(&ctx,1), calls, slots) ||
        !slots.empty())
        return false;

    // A previous indirect call separates earlier outgoing stack stores
    // from subsequent calls; do not reuse stale signatures.
    ctx.after = {
        0x48,0x89,0x44,0x24,0x20,
        0xff,0x50,0x10,
        0xff,0x50,0x08
    };
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1), calls) ||
        calls.size() != 2 ||
        !scan_xg_postcall_stack_writes(
            std::span<const XgCallContext>(&ctx,1), calls, slots) ||
        slots.size() != 1 ||
        slots[0].indirect_call_rva != 0x100a)
        return false;

    // Wrong source metadata fails atomically.
    auto corrupted = calls;
    corrupted[0].displacement = 0x18;
    if (scan_xg_postcall_stack_writes(
            std::span<const XgCallContext>(&ctx,1), corrupted, slots) ||
        slots.size() != 1 || slots[0].stack_offset != 0x20)
        return false;
    // Duplicate section data must not duplicate evidence.
    const std::array<XgCallContext,2> duplicates{{ctx,ctx}};
    if (!scan_xg_postcall_stack_writes(duplicates, calls, slots) ||
        slots.size() != 1)
        return false;
    ctx.after.resize(4097);
    if (scan_xg_postcall_stack_writes(
            std::span<const XgCallContext>(&ctx,1), calls, slots) ||
        slots.size() != 1)
        return false;

    // Bound synthetic input beyond the search window: no inferred stores.
    ctx.after = {0x48,0x89,0x44,0x24,0x20};
    ctx.after.resize(40,0x90);
    ctx.after.insert(ctx.after.end(),{0xff,0x50,0x08});
    if (!scan_xg_indirect_postcalls(
            std::span<const XgCallContext>(&ctx,1),calls) ||
        !scan_xg_postcall_stack_writes(
            std::span<const XgCallContext>(&ctx,1),calls,slots) ||
        !slots.empty())
        return false;
    return true;
}
} // namespace scarlett::analysis
