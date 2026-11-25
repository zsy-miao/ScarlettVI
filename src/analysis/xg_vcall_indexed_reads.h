#pragma once
#include "xg_vcall_candidates.h"
#include <cstdint>
#include <span>
#include <vector>

namespace scarlett::analysis {

// Bounded file-byte pattern evidence for stack reads after +0x10 dispatch.
// It does NOT prove the callee wrote the data, the index value, layout size,
// execution path, pointer aliasing, object identity, or an Xbox ABI.
struct XgPostcallIndexedRead {
    uint32_t source_create_rva = 0;
    uint32_t dispatch_rva = 0;
    uint32_t read_rva = 0;
    uint32_t passed_rsp_offset = 0;
    uint32_t indexed_rsp_displacement = 0;
    uint32_t relative_displacement = 0;
    uint8_t destination_gpr = 0;
    uint8_t width_bytes = 0;
    uint8_t bytes_after_dispatch = 0;
    bool copied_immediately_to_rsi = false;
    uint32_t copy_rva = 0;  // meaningful only when copy is present
    uint32_t rsi_field_offset = 0; // meaningful only when copy is present
};

// Verifies each supplied FF/2 dispatch candidate and RDX stack-address
// preparation using existing scanners. Examines at most 96 following bytes,
// stopping before a subsequent verified indirect call. Finds *only* MOV
// r32/r64,[RSP+RAX+disp8/disp32] with scale 1 and nonnegative displacement
// lying within [RDX's RSP displacement, displacement + 0x70].
// It optionally recognizes an immediate same-width MOV [RSI+disp8],reg
// of the freshly loaded destination register. Does not prove origin.
// Returns false without altering output on invalid input.
bool scan_xg_postcall_indexed_reads(
    std::span<const XgCallContext> contexts,
    std::span<const XgIndirectCallCandidate> indirect,
    std::vector<XgPostcallIndexedRead>& output) noexcept;

bool verify_xg_postcall_indexed_read_fixtures() noexcept;
} // namespace scarlett::analysis
