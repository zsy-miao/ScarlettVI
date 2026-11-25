#include "xg_vcall_indexed_reads.h"
#include "xg_vcall_arguments.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace scarlett::analysis {
namespace {
struct Decoded {
    uint8_t destination = 0;
    uint8_t width = 0;
    uint32_t displacement = 0;
    size_t length = 0;
};
std::optional<Decoded> decode_indexed_read(
    std::span<const uint8_t> bytes, size_t at, size_t limit) noexcept {
    if (limit > bytes.size() || at >= limit)
        return std::nullopt;
    const size_t start = at;
    uint8_t rex = 0;
    if (bytes[at] >= 0x40u && bytes[at] <= 0x4fu) {
        rex = bytes[at++];
        if (at == limit)
            return std::nullopt;
    }
    if (bytes[at++] != 0x8bu || limit - at < 2u)
        return std::nullopt;
    const uint8_t modrm = bytes[at++];
    const uint8_t sib = bytes[at++];
    const uint8_t mode = modrm >> 6;
    // Exact RSP base (no REX.B), RAX index (no REX.X), scale *1.
    if ((mode != 1u && mode != 2u) ||
        (modrm & 7u) != 4u || sib != 0x04u ||
        (rex & 3u) != 0u)
        return std::nullopt;
    uint32_t displacement = 0;
    if (mode == 1u) {
        if (at >= limit || bytes[at] >= 0x80u)
            return std::nullopt; // negative signed disp8
        displacement = bytes[at++];
    } else {
        if (limit - at < 4u)
            return std::nullopt;
        for (unsigned i = 0; i < 4; ++i)
            displacement |= uint32_t{bytes[at++]} << (i * 8u);
    }
    if (displacement > 4096u)
        return std::nullopt;
    return Decoded{
        static_cast<uint8_t>(((modrm >> 3) & 7u) |
                             ((rex & 4u) ? 8u : 0u)),
        static_cast<uint8_t>((rex & 8u) ? 8u : 4u),
        displacement, at - start
    };
}
// A precisely adjacent, same-width transfer from the freshly loaded GPR
// to an RSI-relative field. No intermediate instruction or index aliasing
// is tolerated; unknown stores remain unreported.
bool decode_immediate_copy_to_rsi(
    std::span<const uint8_t> bytes, size_t at, size_t limit,
    const Decoded& source, uint32_t& target_offset) noexcept {
    if (limit > bytes.size() || at >= limit)
        return false;
    uint8_t rex = 0;
    if (bytes[at] >= 0x40u && bytes[at] <= 0x4fu) {
        rex = bytes[at++];
        if (at == limit)
            return false;
    }
    if ((rex & 3u) != 0u ||
        ((rex & 8u) != 0u) != (source.width == 8u) ||
        bytes[at++] != 0x89u || limit - at < 2u)
        return false;
    const uint8_t modrm = bytes[at++];
    if ((modrm >> 6) != 1u || (modrm & 7u) != 6u ||
        static_cast<uint8_t>(((modrm >> 3) & 7u) |
                             ((rex & 4u) ? 8u : 0u)) !=
            source.destination)
        return false;
    const uint8_t disp8 = bytes[at];
    if (disp8 >= 0x80u)
        return false;
    target_offset = disp8;
    return true;
}
bool same_candidate(const XgIndirectCallCandidate& a,
                    const XgIndirectCallCandidate& b) noexcept {
    return a.source_call_rva == b.source_call_rva &&
           a.source_import == b.source_import &&
           a.candidate_rva == b.candidate_rva &&
           a.displacement == b.displacement &&
           a.base_register == b.base_register &&
           a.instruction_bytes == b.instruction_bytes;
}
} // namespace

bool scan_xg_postcall_indexed_reads(
    std::span<const XgCallContext> contexts,
    std::span<const XgIndirectCallCandidate> indirect,
    std::vector<XgPostcallIndexedRead>& output) noexcept {
    try {
        std::vector<XgIndirectCallCandidate> verified;
        if (!scan_xg_indirect_postcalls(contexts, verified))
            return false;
        for (const auto& candidate : indirect) {
            if (std::none_of(verified.begin(), verified.end(),
                [&](const auto& v) { return same_candidate(candidate, v); }))
                return false;
        }
        std::vector<XgArgSetupCandidate> argument_setups;
        if (!scan_xg_dispatch_argument_setups(contexts, indirect, argument_setups))
            return false;
        std::vector<XgPostcallIndexedRead> found;
        for (const auto& call : indirect) {
            if (call.displacement != 0x10u)
                continue;
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
            const uint64_t base = uint64_t{context->call_rva} + 5u;
            const uint64_t call_end =
                uint64_t{call.candidate_rva} + call.instruction_bytes;
            if (call_end < base || call_end > base + context->after.size())
                return false;
            const XgArgSetupCandidate* rdx = nullptr;
            for (const auto& setup : argument_setups)
                if (setup.source_call_rva == call.source_call_rva &&
                    setup.indirect_call_rva == call.candidate_rva &&
                    setup.form == XgArgSetupForm::lea_rdx_from_rsp)
                    rdx = &setup;
            if (!rdx)
                continue; // unknown argument setup, not an error
            const uint32_t rsp_offset = rdx->displacement;
            const size_t after = static_cast<size_t>(call_end - base);
            size_t limit =
                std::min(context->after.size(), after + size_t{96});
            for (const auto& next : verified) {
                if (next.source_call_rva == call.source_call_rva &&
                    next.source_import == call.source_import &&
                    uint64_t{next.candidate_rva} >= call_end &&
                    uint64_t{next.candidate_rva} < base + limit)
                    limit = std::min(limit, static_cast<size_t>(
                        next.candidate_rva - base));
            }
            for (size_t pos = after; pos < limit; ++pos) {
                const auto decoded =
                    decode_indexed_read(context->after, pos, limit);
                if (!decoded ||
                    decoded->displacement < rsp_offset ||
                    uint64_t{decoded->displacement} >
                        uint64_t{rsp_offset} + 0x70u)
                    continue;
                const uint64_t rva = base + pos;
                if (rva > UINT32_MAX)
                    return false;
                XgPostcallIndexedRead item{
                    call.source_call_rva,
                    call.candidate_rva,
                    static_cast<uint32_t>(rva),
                    rsp_offset,
                    decoded->displacement,
                    decoded->displacement - rsp_offset,
                    decoded->destination,
                    decoded->width,
                    static_cast<uint8_t>(rva -
                        static_cast<uint64_t>(call.candidate_rva) -
                        call.instruction_bytes)
                };
                uint32_t target_offset = 0;
                if (decode_immediate_copy_to_rsi(
                        context->after, pos + decoded->length, limit,
                        *decoded, target_offset)) {
                    item.copied_immediately_to_rsi = true;
                    item.copy_rva =
                        static_cast<uint32_t>(rva + decoded->length);
                    item.rsi_field_offset = target_offset;
                }
                found.push_back(item);
                // Suppress the overlapping opcode start of REX-prefixed MOV.
                pos += decoded->length - 1;
            }
        }
        std::sort(found.begin(), found.end(),
                  [](const auto& a, const auto& b) {
                      if (a.source_create_rva != b.source_create_rva)
                          return a.source_create_rva < b.source_create_rva;
                      if (a.dispatch_rva != b.dispatch_rva)
                          return a.dispatch_rva < b.dispatch_rva;
                      return a.read_rva < b.read_rva;
                  });
        found.erase(std::unique(found.begin(), found.end(),
                    [](const auto& a, const auto& b) {
                        return a.source_create_rva == b.source_create_rva &&
                               a.dispatch_rva == b.dispatch_rva &&
                               a.read_rva == b.read_rva;
                    }), found.end());
        output = std::move(found);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::analysis
