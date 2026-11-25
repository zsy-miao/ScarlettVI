#pragma once
#include "xg_vcall_candidates.h"
#include <cstdint>
#include <span>
#include <vector>

namespace scarlett::analysis {

// Bounded read-only pattern evidence; not a complete instruction decoder,
// dataflow proof, Xbox ABI, or known parameter-signature description.
struct XgStackWriteCandidate {
    uint32_t source_call_rva = 0;
    uint32_t indirect_call_rva = 0;
    uint32_t store_rva = 0;
    uint32_t stack_offset = 0;
    uint8_t source_register = 0;  // 0..15
    uint8_t width_bytes = 0;      // 4 or 8
    uint8_t gap_to_call_bytes = 0;
};

// Scan at most 32 bytes preceding each verified indirect call for MOV
// dword/qword ptr [RSP+aligned 0x20..0x80], GPR. Does not infer register
// contents. The latest write to each slot in the window wins; a preceding
// indirect call acts as a conservative clobber barrier. Atomic on failure.
bool scan_xg_postcall_stack_writes(
    std::span<const XgCallContext> contexts,
    std::span<const XgIndirectCallCandidate> indirect,
    std::vector<XgStackWriteCandidate>& found) noexcept;

bool verify_xg_postcall_stack_write_fixtures() noexcept;
} // namespace scarlett::analysis
