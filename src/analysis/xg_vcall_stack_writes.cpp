#include "xg_vcall_stack_writes.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace scarlett::analysis {
namespace {
struct ParsedStore {
    uint32_t offset = 0;
    uint8_t reg = 0;
    uint8_t width = 0;
    size_t length = 0;
};

bool parse_stack_store(std::span<const uint8_t> bytes, size_t at,
                       size_t end, ParsedStore& parsed) noexcept {
    if (end > bytes.size() || at >= end)
        return false;
    const size_t begin = at;
    uint8_t rex = 0;
    if (bytes[at] >= 0x40 && bytes[at] <= 0x4f) {
        rex = bytes[at++];
        if (at >= end)
            return false;
    }
    if (bytes[at++] != 0x89 || end - at < 2)
        return false;
    const uint8_t modrm = bytes[at++];
    const uint8_t sib = bytes[at++];
    const uint8_t mode = modrm >> 6;
    if ((mode != 1 && mode != 2) || (modrm & 7u) != 4 ||
        sib != 0x24 || (rex & 3u) != 0)
        return false;
    const size_t disp_len = mode == 1 ? 1 : 4;
    if (end - at < disp_len)
        return false;
    uint32_t offset = 0;
    if (mode == 1) {
        if (bytes[at] >= 0x80)
            return false; // negative 8-bit displacement
        offset = bytes[at++];
    } else {
        for (size_t i = 0; i < 4; ++i)
            offset |= uint32_t{bytes[at++]} << (i * 8u);
    }
    if (offset < 0x20 || offset > 0x80 || (offset & 7u) != 0)
        return false;
    parsed = {offset,
              static_cast<uint8_t>(((modrm >> 3) & 7u) |
                                   ((rex & 4u) ? 8u : 0u)),
              static_cast<uint8_t>((rex & 8u) ? 8 : 4),
              at - begin};
    return true;
}
} // namespace

bool scan_xg_postcall_stack_writes(
    std::span<const XgCallContext> contexts,
    std::span<const XgIndirectCallCandidate> indirect,
    std::vector<XgStackWriteCandidate>& found) noexcept {
    try {
        std::vector<XgIndirectCallCandidate> verified;
        if (!scan_xg_indirect_postcalls(contexts, verified))
            return false;
        std::vector<XgStackWriteCandidate> next;
        for (const auto& hit : indirect) {
            const XgCallContext* ctx = nullptr;
            for (const auto& context : contexts)
                if (context.call_rva == hit.source_call_rva &&
                    context.symbol == hit.source_import) {
                    ctx = &context;
                    break;
                }
            if (!ctx)
                return false;
            const bool confirmed = std::any_of(
                verified.begin(), verified.end(), [&](const auto& v) {
                    return v.source_call_rva == hit.source_call_rva &&
                           v.source_import == hit.source_import &&
                           v.candidate_rva == hit.candidate_rva &&
                           v.displacement == hit.displacement &&
                           v.base_register == hit.base_register &&
                           v.instruction_bytes == hit.instruction_bytes;
                });
            if (!confirmed)
                return false;
            const uint64_t base = uint64_t{ctx->call_rva} + 5u;
            if (hit.candidate_rva < base ||
                uint64_t{hit.candidate_rva} >= base + ctx->after.size())
                return false;
            const size_t end = static_cast<size_t>(hit.candidate_rva - base);
            size_t first = end > 32 ? end - 32 : 0;
            for (const auto& previous : verified) {
                if (previous.source_call_rva == hit.source_call_rva &&
                    previous.source_import == hit.source_import &&
                    previous.candidate_rva < hit.candidate_rva) {
                    const uint64_t previous_end =
                        uint64_t{previous.candidate_rva} +
                        previous.instruction_bytes;
                    if (previous_end <= hit.candidate_rva &&
                        previous_end >= base)
                        first = std::max(first, static_cast<size_t>(
                            previous_end - base));
                }
            }
            // Walk forward, overwriting previous pattern evidence for an
            // identical stack slot with its nearest qualifying store.
            for (size_t i = first; i < end; ++i) {
                ParsedStore store;
                if (!parse_stack_store(ctx->after, i, end, store))
                    continue;
                const XgStackWriteCandidate candidate{
                    hit.source_call_rva, hit.candidate_rva,
                    static_cast<uint32_t>(base + i),
                    store.offset, store.reg, store.width,
                    static_cast<uint8_t>(end - i - store.length)
                };
                const auto existing = std::find_if(
                    next.begin(), next.end(), [&](const auto& item) {
                        return item.source_call_rva == candidate.source_call_rva &&
                               item.indirect_call_rva == candidate.indirect_call_rva &&
                               item.stack_offset == candidate.stack_offset;
                    });
                if (existing == next.end())
                    next.push_back(candidate);
                else if (candidate.store_rva > existing->store_rva)
                    *existing = candidate;
                // Do not reinterpret the opcode immediately after a REX
                // prefix as a second, narrower 32-bit store pattern.
                i += store.length - 1;
            }
        }
        std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) {
            if (a.source_call_rva != b.source_call_rva)
                return a.source_call_rva < b.source_call_rva;
            if (a.indirect_call_rva != b.indirect_call_rva)
                return a.indirect_call_rva < b.indirect_call_rva;
            return a.stack_offset < b.stack_offset;
        });
        found = std::move(next);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::analysis
