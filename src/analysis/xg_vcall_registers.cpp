#include "xg_vcall_registers.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace scarlett::analysis {
namespace {
struct DecodedPattern {
    XgRegPatternKind kind;
    uint8_t dest;
    uint8_t source;
    uint32_t offset;
    size_t bytes;
};

std::optional<DecodedPattern> parse_pattern(
    std::span<const uint8_t> bytes, size_t start, size_t limit) noexcept {
    if (limit > bytes.size() || start >= limit || limit - start < 3)
        return std::nullopt;
    const uint8_t rex = bytes[start];
    if (rex < 0x48 || rex > 0x4f)
        return std::nullopt; // REX.W required
    const uint8_t opcode = bytes[start + 1];
    const uint8_t modrm = bytes[start + 2];
    if ((opcode == 0x8b || opcode == 0x89) &&
        (modrm & 0xc0) == 0xc0) { // MOV r64,r64
        const uint8_t reg =
            static_cast<uint8_t>(((modrm >> 3) & 7u) |
                                 ((rex & 4u) ? 8u : 0u));
        const uint8_t rm =
            static_cast<uint8_t>((modrm & 7u) |
                                 ((rex & 1u) ? 8u : 0u));
        return DecodedPattern{
            XgRegPatternKind::register_copy,
            opcode == 0x8b ? reg : rm,
            opcode == 0x8b ? rm : reg,
            0, 3
        };
    }
    if (opcode != 0x8d || (modrm & 7u) != 4u ||
        (rex & 3u) != 0u) // LEA from RSP, unindexed
        return std::nullopt;
    if (limit - start < 4 || bytes[start + 3] != 0x24)
        return std::nullopt;
    const uint8_t mode = modrm >> 6;
    size_t cursor = start + 4;
    uint32_t displacement = 0;
    if (mode == 1) {
        if (cursor >= limit || bytes[cursor] >= 0x80)
            return std::nullopt;
        displacement = bytes[cursor++];
    } else if (mode == 2) {
        if (limit - cursor < 4)
            return std::nullopt;
        for (unsigned i = 0; i < 4; ++i)
            displacement |= uint32_t{bytes[cursor++]} << (8u * i);
    } else if (mode != 0) {
        return std::nullopt;
    }
    if (displacement > 4096)
        return std::nullopt;
    return DecodedPattern{
        XgRegPatternKind::stack_address,
        static_cast<uint8_t>(((modrm >> 3) & 7u) |
                             ((rex & 4u) ? 8u : 0u)),
        0, displacement, cursor - start
    };
}

bool pattern_less(const XgRegisterPattern& a,
                  const XgRegisterPattern& b) noexcept {
    if (a.source_call_rva != b.source_call_rva)
        return a.source_call_rva < b.source_call_rva;
    if (a.indirect_call_rva != b.indirect_call_rva)
        return a.indirect_call_rva < b.indirect_call_rva;
    if (a.instruction_rva != b.instruction_rva)
        return a.instruction_rva < b.instruction_rva;
    return a.destination_gpr < b.destination_gpr;
}
} // namespace

bool scan_xg_local_register_patterns(
    std::span<const XgCallContext> contexts,
    std::span<const XgIndirectCallCandidate> indirect,
    std::vector<XgRegisterPattern>& out) noexcept {
    try {
        std::vector<XgIndirectCallCandidate> verified;
        if (!scan_xg_indirect_postcalls(contexts, verified))
            return false;
        std::vector<XgRegisterPattern> observed;
        for (const auto& call : indirect) {
            const bool valid = std::any_of(
                verified.begin(), verified.end(), [&](const auto& v) {
                    return v.source_call_rva == call.source_call_rva &&
                           v.source_import == call.source_import &&
                           v.candidate_rva == call.candidate_rva &&
                           v.displacement == call.displacement &&
                           v.base_register == call.base_register &&
                           v.instruction_bytes == call.instruction_bytes;
                });
            if (!valid)
                return false;
            // The first-stage dispatch probe reports other offsets too.
            // This targeted research module only models the +0x50 window.
            if (call.displacement != 0x50)
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
            const uint64_t first_rva = uint64_t{context->call_rva} + 5;
            if (call.candidate_rva < first_rva ||
                uint64_t{call.candidate_rva} >=
                    first_rva + context->after.size())
                return false;
            const size_t end = static_cast<size_t>(
                call.candidate_rva - first_rva);
            size_t begin = end > 96 ? end - 96 : 0;
            for (const auto& other : verified) {
                if (other.source_call_rva != call.source_call_rva ||
                    other.source_import != call.source_import ||
                    other.candidate_rva >= call.candidate_rva)
                    continue;
                const uint64_t prev_end =
                    uint64_t{other.candidate_rva} + other.instruction_bytes;
                if (prev_end <= call.candidate_rva && prev_end >= first_rva)
                    begin = std::max(begin, static_cast<size_t>(
                        prev_end - first_rva));
            }
            for (size_t i = begin; i < end; ++i) {
                const auto decoded = parse_pattern(context->after, i, end);
                if (!decoded)
                    continue;
                observed.push_back({
                    call.source_call_rva,
                    call.candidate_rva,
                    static_cast<uint32_t>(first_rva + i),
                    decoded->kind,
                    decoded->dest,
                    decoded->source,
                    decoded->offset
                });
                // Never reinterpret the interior of a recognized REX
                // instruction as a second independent pattern.
                i += decoded->bytes - 1;
            }
        }
        std::sort(observed.begin(), observed.end(), pattern_less);
        observed.erase(
            std::unique(observed.begin(), observed.end(),
                        [](const auto& a, const auto& b) {
                            return a.source_call_rva == b.source_call_rva &&
                                   a.indirect_call_rva == b.indirect_call_rva &&
                                   a.instruction_rva == b.instruction_rva &&
                                   a.destination_gpr == b.destination_gpr;
                        }),
            observed.end());
        out = std::move(observed);
        return true;
    } catch (...) {
        return false;
    }
}

bool resolve_xg_stack_address_pattern(
    std::span<const XgRegisterPattern> patterns,
    uint32_t source_call_rva,
    uint32_t indirect_call_rva,
    uint8_t register_at_use,
    uint32_t use_rva,
    uint32_t& stack_offset) noexcept {
    if (register_at_use >= 16)
        return false;
    // A bounded local chain only: no allocations, recursion or guest memory
    // reads, and no return of speculative output on failed lookup.
    std::array<bool, 16> visited{};
    uint8_t reg = register_at_use;
    for (size_t depth = 0; depth < visited.size(); ++depth) {
        if (visited[reg])
            return false;
        visited[reg] = true;
        const XgRegisterPattern* latest = nullptr;
        for (const auto& p : patterns) {
            if (p.source_call_rva != source_call_rva ||
                p.indirect_call_rva != indirect_call_rva ||
                p.destination_gpr != reg || p.instruction_rva >= use_rva)
                continue;
            if (!latest || p.instruction_rva > latest->instruction_rva)
                latest = &p;
        }
        if (!latest)
            return false;
        if (latest->kind == XgRegPatternKind::stack_address) {
            stack_offset = latest->stack_offset;
            return true;
        }
        if (latest->kind != XgRegPatternKind::register_copy ||
            latest->source_gpr >= 16)
            return false;
        reg = latest->source_gpr;
        use_rva = latest->instruction_rva;
    }
    return false;
}
} // namespace scarlett::analysis
