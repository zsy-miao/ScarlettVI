#pragma once
#include "xg_vcall_candidates.h"
#include <cstdint>
#include <span>
#include <vector>

namespace scarlett::analysis {

// Candidate local x64 argument-setup *byte pattern* near an XG post-create
// indirect-call site. NOT a complete decoder, reaching-definition proof,
// verified Xbox ABI, or evidence of the function's semantic purpose.
enum class XgArgSetupForm : uint8_t {
    mov_rcx_from_memory,
    lea_rdx_from_rsp
};

struct XgArgSetupCandidate {
    uint32_t source_call_rva = 0;
    uint32_t indirect_call_rva = 0;
    uint32_t setup_rva = 0;
    XgArgSetupForm form = XgArgSetupForm::mov_rcx_from_memory;
    uint8_t memory_base_register = 0;
    uint32_t displacement = 0;
    uint8_t bytes_between_setup_and_call = 0;
};

// Examines at most 24 preceding bytes of the verified post-call contexts.
// Finds only nearby REX.W MOV RCX,[base+disp] and LEA RDX,[RSP+disp] forms,
// with optional no-index SIB and nonnegative displacement <= 4096.
// Reports nearest pattern for each form and call. Does not establish liveness
// or address contents. Returns false atomically on invalid contexts/candidates.
bool scan_xg_dispatch_argument_setups(
    std::span<const XgCallContext> contexts,
    std::span<const XgIndirectCallCandidate> indirect_calls,
    std::vector<XgArgSetupCandidate>& out) noexcept;

bool verify_xg_dispatch_argument_setup_fixtures() noexcept;
} // namespace scarlett::analysis
