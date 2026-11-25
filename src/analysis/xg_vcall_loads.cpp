#include "xg_vcall_loads.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace scarlett::analysis {
bool scan_xg_table_load_candidates(
    std::span<const XgCallContext> contexts,
    std::span<const XgIndirectCallCandidate> indirect,
    std::vector<XgTableLoadCandidate>& found) noexcept {
    try {
        std::vector<XgTableLoadCandidate> result;
        for (const auto& context : contexts) {
            if (context.call_bytes[0] != 0xe8 ||
                context.after.size() > 4096 ||
                uint64_t{context.call_rva} + 5 + context.after.size() >
                    UINT32_MAX)
                return false;
        }
        for (const auto& call : indirect) {
            const XgCallContext* source = nullptr;
            for (const auto& ctx : contexts) {
                if (ctx.call_rva == call.source_call_rva &&
                    ctx.symbol == call.source_import) {
                    source = &ctx;
                    break;
                }
            }
            if (!source || call.base_register >= 16 ||
                call.instruction_bytes < 3 ||
                call.instruction_bytes > 7)
                return false;
            const uint64_t start_rva = uint64_t{source->call_rva} + 5;
            if (call.candidate_rva < start_rva ||
                uint64_t{call.candidate_rva} + call.instruction_bytes >
                    start_rva + source->after.size())
                return false;

            const size_t offset =
                static_cast<size_t>(call.candidate_rva - start_rva);
            const auto& bytes = source->after;
            // Revalidate the candidate against the supplied byte window;
            // do not silently trust caller-provided register/offset metadata.
            size_t opcode = offset;
            uint8_t call_rex = 0;
            if (bytes[opcode] >= 0x40 && bytes[opcode] <= 0x4f) {
                call_rex = bytes[opcode++];
            }
            if (opcode + 1 >= bytes.size() || bytes[opcode] != 0xff)
                return false;
            const uint8_t call_modrm = bytes[opcode + 1];
            const uint8_t mode = call_modrm >> 6;
            const uint8_t rm = call_modrm & 7u;
            if (((call_modrm >> 3) & 7u) != 2u ||
                (mode != 1u && mode != 2u) || rm == 4u ||
                (rm | ((call_rex & 1u) ? 8u : 0u)) != call.base_register)
                return false;
            const size_t disp_width = mode == 1 ? 1 : 4;
            if (call.instruction_bytes != opcode - offset + 2 + disp_width ||
                bytes.size() - (opcode + 2) < disp_width)
                return false;
            uint32_t displacement = 0;
            for (size_t j = 0; j < disp_width; ++j)
                displacement |= uint32_t{bytes[opcode + 2 + j]} << (8 * j);
            if ((mode == 1 && bytes[opcode + 2] >= 0x80) ||
                displacement > 4096 || displacement != call.displacement ||
                (displacement & 7u) != 0)
                return false;
            // A short look-back is intentionally only a local signature
            // check; instructions between load and call are NOT decoded.
            const size_t first = offset > 32 ? offset - 32 : 0;
            for (size_t end = offset; end >= first + 3; --end) {
                const size_t i = end - 3;
                if (bytes[i + 1] != 0x8b)
                    continue;
                const uint8_t rex = bytes[i];
                const uint8_t modrm = bytes[i + 2];
                if (rex < 0x48 || rex > 0x4f ||
                    (modrm & 0xc0) != 0x00 ||
                    (modrm & 7) == 4 || (modrm & 7) == 5)
                    continue;
                const uint8_t destination =
                    static_cast<uint8_t>(((modrm >> 3) & 7u) |
                                         ((rex & 4u) ? 8u : 0u));
                if (destination != call.base_register)
                    continue;
                const uint8_t source_register =
                    static_cast<uint8_t>((modrm & 7u) |
                                         ((rex & 1u) ? 8u : 0u));
                const size_t distance = offset - (i + 3);
                // 32-byte look-back makes this fit in an 8-bit field.
                result.push_back({
                    call.source_call_rva,
                    call.candidate_rva,
                    static_cast<uint32_t>(start_rva + i),
                    call.displacement,
                    destination,
                    source_register,
                    static_cast<uint8_t>(distance)
                });
                break; // nearest preceding matching producer pattern
            }
        }
        std::sort(result.begin(), result.end(),
                  [](const auto& a, const auto& b) {
                      if (a.source_call_rva != b.source_call_rva)
                          return a.source_call_rva < b.source_call_rva;
                      if (a.indirect_call_rva != b.indirect_call_rva)
                          return a.indirect_call_rva < b.indirect_call_rva;
                      return a.load_rva < b.load_rva;
                  });
        result.erase(std::unique(result.begin(), result.end(),
                  [](const auto& a, const auto& b) {
                      return a.source_call_rva == b.source_call_rva &&
                             a.indirect_call_rva == b.indirect_call_rva &&
                             a.load_rva == b.load_rva;
                  }), result.end());
        found = std::move(result);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::analysis
