#pragma once
#include "../pe/pe_file.h"
#include <array>
#include <cstdint>
#include <string>
#include <span>
#include <string_view>
#include <vector>

namespace scarlett::analysis {
// Static *byte-pattern* evidence from PE executable sections only.
// E8/FF25 candidates are not fully instruction-boundary decoded; confirm
// interesting sites with a trusted disassembler. NEVER calls import targets.
struct XgCallsiteEvidence {
    std::string_view symbol;
    bool imported = false;
    uint32_t iat_rva = 0;
    std::vector<uint32_t> trampoline_rvas;
    std::vector<uint32_t> direct_rel32_call_rvas;
};
struct XgCallsiteReport {
    uint64_t image_base = 0;
    std::array<XgCallsiteEvidence, 2> symbols{{
        {"XGCreateTextureComputer"}, {"XGSuggestSwizzleMode"}
    }};
    size_t executable_bytes_scanned = 0;
};
struct ExecutableBytes {
    uint32_t rva = 0;
    std::span<const uint8_t> bytes;
};
// Bounded, read-only instruction context for manual ABI investigation.
// This deliberately contains raw file bytes, not inferred instruction boundaries
// or decoded console ABI fields. Never persist title bytes in the repository.
struct XgCallContext {
    std::string_view symbol;
    uint32_t call_rva = 0;
    uint32_t target_trampoline_rva = 0;
    uint32_t section_rva = 0;
    std::vector<uint8_t> before;
    std::array<uint8_t, 5> call_bytes{};
    std::vector<uint8_t> after;
};
// Pure, bounded window extractor: validates E8 displacement against a known
// trampoline and refuses ambiguous, missing or non-executable file ranges.
bool gather_xg_call_contexts(
    std::span<const ExecutableBytes> sections,
    const XgCallsiteReport& report,
    std::vector<XgCallContext>& contexts,
    size_t before_bytes = 96,
    size_t after_bytes = 48) noexcept;
bool probe_xg_call_contexts(
    const scarlett::PeFile& file,
    const XgCallsiteReport& report,
    std::vector<XgCallContext>& contexts,
    size_t before_bytes = 96,
    size_t after_bytes = 48) noexcept;
bool verify_xg_context_fixtures() noexcept;
// Pure scanner for generated fixtures or independently validated PE sections.
// Reports RIP-relative FF25 import stubs and E8 rel32 call-site candidates.
bool scan_xg_executable_sections(
    std::span<const ExecutableBytes> sections,
    XgCallsiteReport& report) noexcept;
bool probe_xg_callsites(const scarlett::PeFile& file,
                        XgCallsiteReport& report) noexcept;
bool verify_xg_callsite_fixtures() noexcept;
} // namespace scarlett::analysis
