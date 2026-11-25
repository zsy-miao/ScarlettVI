#pragma once
#include "xg_vcall_candidates.h"
#include <cstdint>
#include <span>
#include <vector>

namespace scarlett::analysis {

// A local x64 MOV r64,[r64] byte-pattern producer for an already discovered
// FF /2 candidate's base register. NOT a full instruction/data-flow decoder,
// and NOT proof of an Xbox vtable, COM interface, or object identity.
struct XgTableLoadCandidate {
    uint32_t source_call_rva = 0;
    uint32_t indirect_call_rva = 0;
    uint32_t load_rva = 0;
    uint32_t indirect_displacement = 0;
    uint8_t table_register = 0;
    uint8_t source_pointer_register = 0;
    uint8_t distance_to_call_bytes = 0;
};

// Searches the preceding at most 32 bytes of a file-backed, verified
// post-create call window for a simple REX.W + 8B + ModRM register load with
// no displacement or SIB. Reports nearest producer-pattern per candidate.
// Missing a producer is not an error; failed validation leaves output intact.
bool scan_xg_table_load_candidates(
    std::span<const XgCallContext> contexts,
    std::span<const XgIndirectCallCandidate> indirect,
    std::vector<XgTableLoadCandidate>& found) noexcept;

bool verify_xg_table_load_fixtures() noexcept;
} // namespace scarlett::analysis
