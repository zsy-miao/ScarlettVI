#pragma once
#include "xg_vcall_candidates.h"
#include <cstdint>
#include <span>
#include <vector>

namespace scarlett::analysis {

// Local byte-pattern evidence of a +0x08 indirect call immediately following
// a +0x10 or +0x50 indirect call. No instruction/dataflow completeness,
// callee identity, ownership/lifetime, or COM Release semantics are claimed.
struct XgDispatchSuccessorEvidence {
    uint32_t source_create_rva = 0;
    uint32_t first_indirect_rva = 0;
    uint32_t next_indirect_rva = 0;
    uint32_t first_dispatch_offset = 0;
    uint32_t reload_rcx_rva = 0;
    uint32_t reload_table_rva = 0;
    uint8_t reload_base_gpr = 0;
    uint32_t reload_displacement = 0;
    bool prior_matching_storage_pattern = false;
    uint32_t prior_load_rva = 0; // meaningful only if matching
};

// Validates candidate metadata against the original bounded post-create
// context, then checks the COMPLETE inter-call gap for two narrowly supported
// instructions: MOV RCX,[base + nonnegative offset], MOV RAX,[RCX]. A bounded
// look-back (<=96 bytes) checks for a previous equivalent RCX load pattern,
// not a guaranteed same live pointer or actual object identity.
// On any invalid input the output remains unchanged.
bool scan_xg_dispatch_successors(
    std::span<const XgCallContext> contexts,
    std::span<const XgIndirectCallCandidate> indirect,
    std::vector<XgDispatchSuccessorEvidence>& output) noexcept;

bool verify_xg_dispatch_successor_fixtures() noexcept;
} // namespace scarlett::analysis
