#pragma once
#include "xg_callsite_probe.h"
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace scarlett::analysis {

// Raw x64 FF /2 indirect-call *byte-pattern* evidence after a verified XG
// creation call. A matching offset is not proof of an instruction boundary,
// actual XG object identity, a vtable, or a COM method contract.
struct XgIndirectCallCandidate {
    std::string_view source_import;
    uint32_t source_call_rva = 0;
    uint32_t candidate_rva = 0;
    uint32_t displacement = 0;
    uint8_t base_register = 0; // x64 GPR index 0..15
    uint8_t instruction_bytes = 0;
};

// Bounded, read-only pattern analysis on already-extracted executable bytes.
// Only positive, qword-aligned [register + disp8/disp32] memory call forms
// with no SIB index are considered. Error leaves 'found' unchanged.
bool scan_xg_indirect_postcalls(
    std::span<const XgCallContext> contexts,
    std::vector<XgIndirectCallCandidate>& found) noexcept;

bool verify_xg_indirect_postcall_fixtures() noexcept;
} // namespace scarlett::analysis
