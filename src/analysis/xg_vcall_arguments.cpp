#include "xg_vcall_arguments.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace scarlett::analysis {
namespace {
struct Parsed {
    XgArgSetupForm form{};
    uint8_t base = 0;
    uint32_t displacement = 0;
    size_t length = 0;
};

// Only the two intentionally narrow supported x64 instruction forms.
// All array reads are bounded and all multi-byte displacements little-endian.
std::optional<Parsed> parse_setup(
    std::span<const uint8_t> bytes, size_t start, size_t limit) noexcept {
    if (limit > bytes.size() || start >= limit || limit - start < 3)
        return std::nullopt;

    const uint8_t rex = bytes[start];
    const uint8_t opcode = bytes[start + 1];
    if ((rex & 0xf8) != 0x48 || (opcode != 0x8b && opcode != 0x8d))
        return std::nullopt;

    const uint8_t modrm = bytes[start + 2];
    const uint8_t mode = modrm >> 6;
    const uint8_t dest = ((modrm >> 3) & 7u) | ((rex & 4u) ? 8u : 0u);
    if (mode == 3 || (opcode == 0x8b && dest != 1) ||
        (opcode == 0x8d && dest != 2))
        return std::nullopt;

    size_t at = start + 3;
    uint8_t base = (modrm & 7u) | ((rex & 1u) ? 8u : 0u);
    if ((modrm & 7u) == 4) {
        if (at == limit)
            return std::nullopt;
        const uint8_t sib = bytes[at++];
        if ((sib & 0xc0) != 0 || ((sib >> 3) & 7u) != 4 ||
            (rex & 2u) != 0)
            return std::nullopt; // no scaled index, or REX.X
        base = (sib & 7u) | ((rex & 1u) ? 8u : 0u);
        if (mode == 0 && (sib & 7u) == 5)
            return std::nullopt; // disp32 absolute, not a GPR
    } else if (mode == 0 && (modrm & 7u) == 5) {
        return std::nullopt; // RIP-relative addressing is not a GPR
    }

    uint32_t displacement = 0;
    if (mode == 1) {
        if (at == limit || bytes[at] >= 0x80)
            return std::nullopt;
        displacement = bytes[at++];
    } else if (mode == 2) {
        if (limit - at < 4)
            return std::nullopt;
        for (size_t b = 0; b < 4; ++b)
            displacement |= uint32_t{bytes[at++]} << (8u * b);
    }
    if (displacement > 4096 ||
        (opcode == 0x8d && base != 4u))
        return std::nullopt; // LEA RDX specifically from RSP

    return Parsed{
        opcode == 0x8b ? XgArgSetupForm::mov_rcx_from_memory
                       : XgArgSetupForm::lea_rdx_from_rsp,
        base, displacement, at - start
    };
}
} // namespace

bool scan_xg_dispatch_argument_setups(
    std::span<const XgCallContext> contexts,
    std::span<const XgIndirectCallCandidate> indirect_calls,
    std::vector<XgArgSetupCandidate>& out) noexcept {
    try {
        std::vector<XgIndirectCallCandidate> verified;
        if (!scan_xg_indirect_postcalls(contexts, verified))
            return false;

        std::vector<XgArgSetupCandidate> found;
        for (const auto& call : indirect_calls) {
            const XgCallContext* context = nullptr;
            for (const auto& ctx : contexts) {
                if (ctx.call_rva == call.source_call_rva &&
                    ctx.symbol == call.source_import) {
                    context = &ctx;
                    break;
                }
            }
            if (!context)
                return false;

            bool matches = false;
            for (const auto& v : verified) {
                if (v.source_call_rva == call.source_call_rva &&
                    v.candidate_rva == call.candidate_rva &&
                    v.displacement == call.displacement &&
                    v.base_register == call.base_register &&
                    v.instruction_bytes == call.instruction_bytes &&
                    v.source_import == call.source_import) {
                    matches = true;
                    break;
                }
            }
            if (!matches)
                return false;

            const uint64_t after_rva = uint64_t{context->call_rva} + 5u;
            if (call.candidate_rva < after_rva ||
                uint64_t{call.candidate_rva} >=
                    after_rva + context->after.size())
                return false;
            const size_t offset = static_cast<size_t>(
                call.candidate_rva - after_rva);
            size_t begin = offset > 24 ? offset - 24 : 0;
            // Conservative barrier: a preceding indirect call can clobber
            // volatile RCX/RDX. Never reuse setup patterns across it.
            // This is not full control-flow or instruction-boundary decoding.
            for (const auto& prior : verified) {
                if (prior.source_call_rva != call.source_call_rva ||
                    prior.source_import != call.source_import ||
                    prior.candidate_rva >= call.candidate_rva)
                    continue;
                const uint64_t prior_end =
                    uint64_t{prior.candidate_rva} + prior.instruction_bytes;
                if (prior_end <= call.candidate_rva &&
                    prior_end >= after_rva)
                    begin = std::max(begin, static_cast<size_t>(
                        prior_end - after_rva));
            }

            std::array<std::optional<XgArgSetupCandidate>, 2> best{};
            for (size_t i = begin; i < offset; ++i) {
                const auto parsed = parse_setup(context->after, i, offset);
                if (!parsed)
                    continue;
                const size_t slot =
                    parsed->form == XgArgSetupForm::mov_rcx_from_memory ? 0 : 1;
                best[slot] = XgArgSetupCandidate{
                    call.source_call_rva,
                    call.candidate_rva,
                    static_cast<uint32_t>(after_rva + i),
                    parsed->form,
                    parsed->base,
                    parsed->displacement,
                    static_cast<uint8_t>(offset - (i + parsed->length))
                };
            }
            for (const auto& item : best)
                if (item)
                    found.push_back(*item);
        }
        std::sort(found.begin(), found.end(),
                  [](const auto& a, const auto& b) {
                      if (a.source_call_rva != b.source_call_rva)
                          return a.source_call_rva < b.source_call_rva;
                      if (a.indirect_call_rva != b.indirect_call_rva)
                          return a.indirect_call_rva < b.indirect_call_rva;
                      return static_cast<unsigned>(a.form) <
                             static_cast<unsigned>(b.form);
                  });
        found.erase(std::unique(found.begin(), found.end(),
                   [](const auto& a, const auto& b) {
                       return a.source_call_rva == b.source_call_rva &&
                              a.indirect_call_rva == b.indirect_call_rva &&
                              a.form == b.form;
                   }), found.end());
        out = std::move(found);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::analysis
