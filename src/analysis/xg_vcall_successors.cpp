#include "xg_vcall_successors.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace scarlett::analysis {
namespace {
struct RcXLoad {
    uint8_t base = 0;
    uint32_t displacement = 0;
    size_t length = 0;
};

// Parse just REX.W MOV RCX,[base(+nonnegative offset)].
// No RIP-relative or scaled/indexed addresses, no memory side effects.
std::optional<RcXLoad> parse_rcx_load(
    std::span<const uint8_t> bytes, size_t pos, size_t end) noexcept {
    if (end > bytes.size() || pos > end || end - pos < 3)
        return std::nullopt;
    const uint8_t rex = bytes[pos];
    if ((rex & 0xfcu) != 0x48u || bytes[pos + 1] != 0x8bu)
        return std::nullopt; // REX.W, no REX.R (destination RCX)
    const uint8_t modrm = bytes[pos + 2];
    if (((modrm >> 3) & 7u) != 1u || (modrm & 0xc0u) == 0xc0u)
        return std::nullopt;
    const uint8_t mode = modrm >> 6;
    size_t offset = pos + 3;
    const uint8_t rm = modrm & 7u;
    uint8_t base = static_cast<uint8_t>(rm | ((rex & 1u) ? 8u : 0u));
    if (rm == 4u) {
        if (offset == end)
            return std::nullopt;
        const uint8_t sib = bytes[offset++];
        if (((sib >> 3) & 7u) != 4u || (rex & 2u) != 0u)
            return std::nullopt; // no index or REX.X
        const uint8_t base_bits = sib & 7u;
        if (mode == 0u && base_bits == 5u)
            return std::nullopt;
        base = static_cast<uint8_t>(base_bits | ((rex & 1u) ? 8u : 0u));
    } else if (mode == 0u && rm == 5u) {
        return std::nullopt; // RIP-relative is not a GPR
    }
    uint32_t displacement = 0;
    if (mode == 1u) {
        if (offset >= end || bytes[offset] >= 0x80u)
            return std::nullopt;
        displacement = bytes[offset++];
    } else if (mode == 2u) {
        if (end - offset < 4)
            return std::nullopt;
        for (unsigned i = 0; i < 4; ++i)
            displacement |= uint32_t{bytes[offset++]} << (8u * i);
    }
    if (displacement > 4096u)
        return std::nullopt;
    return RcXLoad{base, displacement, offset - pos};
}
bool table_load_rax_from_rcx(
    std::span<const uint8_t> bytes, size_t pos, size_t end) noexcept {
    return end <= bytes.size() && pos <= end &&
           end - pos == 3 &&
           bytes[pos] == 0x48 && bytes[pos + 1] == 0x8b &&
           bytes[pos + 2] == 0x01;
}
bool same_call(const XgIndirectCallCandidate& a,
               const XgIndirectCallCandidate& b) noexcept {
    return a.source_call_rva == b.source_call_rva &&
           a.source_import == b.source_import &&
           a.candidate_rva == b.candidate_rva &&
           a.displacement == b.displacement &&
           a.base_register == b.base_register &&
           a.instruction_bytes == b.instruction_bytes;
}
bool less_than(const XgDispatchSuccessorEvidence& a,
               const XgDispatchSuccessorEvidence& b) noexcept {
    if (a.source_create_rva != b.source_create_rva)
        return a.source_create_rva < b.source_create_rva;
    if (a.first_indirect_rva != b.first_indirect_rva)
        return a.first_indirect_rva < b.first_indirect_rva;
    return a.next_indirect_rva < b.next_indirect_rva;
}
} // namespace

bool scan_xg_dispatch_successors(
    std::span<const XgCallContext> contexts,
    std::span<const XgIndirectCallCandidate> indirect,
    std::vector<XgDispatchSuccessorEvidence>& output) noexcept {
    try {
        std::vector<XgIndirectCallCandidate> verified;
        if (!scan_xg_indirect_postcalls(contexts, verified))
            return false;

        // A caller cannot supply uncorroborated or conflicting call metadata.
        for (const auto& candidate : indirect) {
            if (std::none_of(verified.begin(), verified.end(),
                             [&](const auto& expected) {
                                 return same_call(candidate, expected);
                             }))
                return false;
        }
        std::vector<XgDispatchSuccessorEvidence> result;
        for (const auto& first : indirect) {
            if (first.displacement != 0x50u &&
                first.displacement != 0x10u)
                continue;
            const XgCallContext* context = nullptr;
            for (const auto& c : contexts)
                if (c.call_rva == first.source_call_rva &&
                    c.symbol == first.source_import) {
                    context = &c;
                    break;
                }
            if (!context)
                return false;
            const uint64_t start_rva = uint64_t{context->call_rva} + 5u;
            const uint64_t first_end =
                uint64_t{first.candidate_rva} + first.instruction_bytes;
            if (first_end < start_rva ||
                first_end > start_rva + context->after.size())
                return false;

            for (const auto& next : indirect) {
                if (next.source_call_rva != first.source_call_rva ||
                    next.source_import != first.source_import ||
                    next.displacement != 8u ||
                    next.base_register != 0 ||
                    next.candidate_rva < first_end ||
                    uint64_t{next.candidate_rva} - first_end > 16u)
                    continue;
                const uint64_t next_start = next.candidate_rva;
                if (next_start >= start_rva + context->after.size())
                    return false;
                const size_t pos = static_cast<size_t>(
                    first_end - start_rva);
                const size_t end = static_cast<size_t>(
                    next_start - start_rva);
                const auto load = parse_rcx_load(context->after, pos, end);
                if (!load || load->length > end - pos)
                    continue;
                const size_t table_pos = pos + load->length;
                if (!table_load_rax_from_rcx(
                        context->after, table_pos, end))
                    continue;

                XgDispatchSuccessorEvidence evidence;
                evidence.source_create_rva = first.source_call_rva;
                evidence.first_indirect_rva = first.candidate_rva;
                evidence.next_indirect_rva = next.candidate_rva;
                evidence.first_dispatch_offset = first.displacement;
                evidence.reload_rcx_rva =
                    static_cast<uint32_t>(first_end);
                evidence.reload_table_rva =
                    static_cast<uint32_t>(start_rva + table_pos);
                evidence.reload_base_gpr = load->base;
                evidence.reload_displacement = load->displacement;

                // A matching earlier signature is not proof that the
                // intervening call leaves memory or the pointer unchanged.
                const size_t first_pos = static_cast<size_t>(
                    first.candidate_rva - start_rva);
                const size_t begin =
                    first_pos > 96 ? first_pos - 96 : 0;
                for (size_t i = begin; i < first_pos; ++i) {
                    const auto earlier = parse_rcx_load(
                        context->after, i, first_pos);
                    if (!earlier)
                        continue;
                    // Only the NEAREST prior recognized RCX load is
                    // relevant, even when from a different storage slot.
                    evidence.prior_matching_storage_pattern =
                        earlier->base == load->base &&
                        earlier->displacement == load->displacement;
                    evidence.prior_load_rva =
                        static_cast<uint32_t>(start_rva + i);
                    i += earlier->length - 1;
                }
                result.push_back(evidence);
            }
        }
        std::sort(result.begin(), result.end(), less_than);
        result.erase(std::unique(result.begin(), result.end(),
                                 [](const auto& a, const auto& b) {
                                     return a.source_create_rva ==
                                                b.source_create_rva &&
                                            a.first_indirect_rva ==
                                                b.first_indirect_rva &&
                                            a.next_indirect_rva ==
                                                b.next_indirect_rva;
                                 }), result.end());
        output = std::move(result);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::analysis
