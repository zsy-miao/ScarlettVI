#pragma once
#include "xg_vcall_candidates.h"
#include <cstdint>
#include <span>
#include <vector>

namespace scarlett::analysis {

// An intentionally partial, read-only register-assignment *pattern* timeline.
// NOT a full x64 instruction decoder, control-flow graph, liveness proof,
// guest execution engine, or verified Xbox object ABI.
enum class XgRegPatternKind : uint8_t { stack_address, register_copy };

struct XgRegisterPattern {
    uint32_t source_call_rva = 0;
    uint32_t indirect_call_rva = 0;
    uint32_t instruction_rva = 0;
    XgRegPatternKind kind = XgRegPatternKind::stack_address;
    uint8_t destination_gpr = 0;
    uint8_t source_gpr = 0;    // for register_copy only
    uint32_t stack_offset = 0; // for stack_address only
};

// Bounded window: at most 96 bytes before each validated FF /2 dispatch
// candidate at table offset +0x50. Reports all recognized REX.W
// LEA r64,[RSP+positive disp] and MOV r64,r64 patterns in chronological
// order, including earlier writes to the same register.
// Prior verified indirect calls form conservative barriers.
// Invalid inputs leave 'out' unchanged.
bool scan_xg_local_register_patterns(
    std::span<const XgCallContext> contexts,
    std::span<const XgIndirectCallCandidate> indirect,
    std::vector<XgRegisterPattern>& out) noexcept;

// Pure data-only query: walk back through recognized register copies to a
// recognized stack-address LEA, respecting the instruction timeline and
// avoiding cycles. Unknown/unrecognized register definitions are NOT detected.
// Returns false if no local matching pattern chain exists.
bool resolve_xg_stack_address_pattern(
    std::span<const XgRegisterPattern> patterns,
    uint32_t source_call_rva,
    uint32_t indirect_call_rva,
    uint8_t register_at_use,
    uint32_t use_rva,
    uint32_t& stack_offset) noexcept;

bool verify_xg_register_pattern_fixtures() noexcept;
} // namespace scarlett::analysis
