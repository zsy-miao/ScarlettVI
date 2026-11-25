#include "xg_vcall_candidates.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

namespace scarlett::analysis {
namespace {
// Recognize a complete, immediately adjacent REX.W LEA r64,[RSP+disp8]
// whose displacement byte is at 'at'. If that final byte resembles a
// REX prefix, do not swallow it into a speculative FF call. This is a
// narrow overlap filter, not a comprehensive x64 decoder.
bool preceding_rsp_lea_disp8(std::span<const uint8_t> bytes,
                              size_t at) noexcept {
    if (at < 4 || at >= bytes.size())
        return false;
    const uint8_t rex = bytes[at - 4];
    const uint8_t modrm = bytes[at - 2];
    return (rex == 0x48 || rex == 0x4c) &&
           bytes[at - 3] == 0x8d &&
           (modrm & 0xc7u) == 0x44u &&
           bytes[at - 1] == 0x24;
}
} // namespace

bool scan_xg_indirect_postcalls(
    std::span<const XgCallContext> contexts,
    std::vector<XgIndirectCallCandidate>& found) noexcept {
    try {
        std::vector<XgIndirectCallCandidate> candidates;
        for (const auto& ctx : contexts) {
            const auto& bytes = ctx.after;
            // Reject unusable/misconfigured windows instead of scanning
            // arbitrary unbounded bytes.
            if (bytes.size() > 4096 || ctx.call_bytes[0] != 0xE8 ||
                uint64_t{ctx.call_rva} + 5 + bytes.size() > UINT32_MAX)
                return false;
            for (size_t i = 0; i < bytes.size(); ++i) {
                size_t cursor = i;
                uint8_t rex = 0;
                if (bytes[cursor] >= 0x40 && bytes[cursor] <= 0x4f) {
                    rex = bytes[cursor];
                    // The FF /2 memory-call form already uses an r/m64
                    // operand. With no SIB index, REX.W is redundant and
                    // REX.X/REX.R add no meaningful base-register identity.
                    // Keep ONLY the B=1 form needed for R8..R15 bases.
                    // Otherwise prefer the later FF opcode as a candidate:
                    // an 0x40/0x48 byte can be the previous instruction's
                    // immediate/displacement, not a true REX prefix.
                    //
                    // REX.B forms are still *byte candidates*, not verified
                    // instruction boundaries. Independently disassemble.
                    if ((rex != 0x41 && rex != 0x49) ||
                        preceding_rsp_lea_disp8(bytes, i))
                        continue;
                    ++cursor;
                    if (cursor >= bytes.size())
                        break;
                }
                if (bytes[cursor] != 0xff || cursor + 1 >= bytes.size())
                    continue;
                const uint8_t modrm = bytes[cursor + 1];
                const uint8_t mode = modrm >> 6;
                const uint8_t field = (modrm >> 3) & 7u;
                const uint8_t rm = modrm & 7u;
                if (field != 2 || (mode != 1 && mode != 2) || rm == 4)
                    continue;
                const size_t disp_bytes = mode == 1 ? 1 : 4;
                if (bytes.size() - (cursor + 2) < disp_bytes)
                    continue;
                int32_t disp = 0;
                if (disp_bytes == 1)
                    disp = static_cast<int8_t>(bytes[cursor + 2]);
                else
                    std::memcpy(&disp, bytes.data() + cursor + 2, 4);
                if (disp < 0 || disp > 4096 || (disp & 7) != 0)
                    continue;
                const uint8_t length =
                    static_cast<uint8_t>(cursor - i + 2 + disp_bytes);
                candidates.push_back({
                    ctx.symbol,
                    ctx.call_rva,
                    static_cast<uint32_t>(ctx.call_rva + 5 + i),
                    static_cast<uint32_t>(disp),
                    static_cast<uint8_t>(rm | ((rex & 1u) ? 8u : 0u)),
                    length
                });
                i += length - 1;
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) {
                      if (a.source_call_rva != b.source_call_rva)
                          return a.source_call_rva < b.source_call_rva;
                      if (a.candidate_rva != b.candidate_rva)
                          return a.candidate_rva < b.candidate_rva;
                      if (a.base_register != b.base_register)
                          return a.base_register < b.base_register;
                      return a.displacement < b.displacement;
                  });
        candidates.erase(std::unique(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) {
                      return a.source_call_rva == b.source_call_rva &&
                             a.candidate_rva == b.candidate_rva &&
                             a.base_register == b.base_register &&
                             a.displacement == b.displacement;
                  }), candidates.end());
        found = std::move(candidates);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett::analysis
