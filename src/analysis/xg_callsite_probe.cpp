#include "xg_callsite_probe.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>

namespace scarlett::analysis {
namespace {
int32_t read_disp32(const uint8_t* p) noexcept {
    int32_t result = 0;
    std::memcpy(&result, p, sizeof(result));
    return result;
}
bool rip_target(uint32_t instruction_rva, uint32_t length,
                int32_t displacement, uint32_t& target) noexcept {
    const int64_t address = int64_t{instruction_rva} + length + displacement;
    if (address < 0 || address > UINT32_MAX) return false;
    target = static_cast<uint32_t>(address);
    return true;
}
bool matches_ascii_no_case(std::string_view a,
                           std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto x = static_cast<unsigned char>(a[i]);
        const auto y = static_cast<unsigned char>(b[i]);
        if (std::tolower(x) != std::tolower(y)) return false;
    }
    return true;
}
void deduplicate(std::vector<uint32_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}
}

// Never reads an IAT *value* or executes code. Only treats known PE IAT
// *addresses* as the targets of FF 25 RIP-relative jump-stub byte patterns.
bool scan_xg_executable_sections(
    std::span<const ExecutableBytes> sections,
    XgCallsiteReport& report) noexcept {
    try {
        for (auto& e : report.symbols) {
            e.trampoline_rvas.clear();
            e.direct_rel32_call_rvas.clear();
        }
        report.executable_bytes_scanned = 0;
        for (const auto& section : sections) {
            if (section.bytes.size() > UINT32_MAX ||
                uint64_t{section.rva} + section.bytes.size() > UINT32_MAX)
                return false;
            report.executable_bytes_scanned += section.bytes.size();
            const auto raw = section.bytes;
            for (size_t i = 0; i + 6 <= raw.size(); ++i) {
                if (raw[i] != 0xFF || raw[i + 1] != 0x25) continue;
                uint32_t target = 0;
                if (!rip_target(section.rva + static_cast<uint32_t>(i),
                                6, read_disp32(raw.data() + i + 2), target))
                    continue;
                for (auto& e : report.symbols)
                    if (e.imported && e.iat_rva == target)
                        e.trampoline_rvas.push_back(
                            section.rva + static_cast<uint32_t>(i));
            }
        }
        for (auto& e : report.symbols) deduplicate(e.trampoline_rvas);

        // The CPU byte E8 is not necessarily an instruction start without
        // full x64 decoding. These are *candidates*, later corroborated
        // with disassembly; do not claim every byte match is executable.
        for (const auto& section : sections) {
            const auto raw = section.bytes;
            for (size_t i = 0; i + 5 <= raw.size(); ++i) {
                if (raw[i] != 0xE8) continue;
                uint32_t target = 0;
                if (!rip_target(section.rva + static_cast<uint32_t>(i),
                                5, read_disp32(raw.data() + i + 1), target))
                    continue;
                for (auto& e : report.symbols)
                    if (std::binary_search(e.trampoline_rvas.begin(),
                                           e.trampoline_rvas.end(), target))
                        e.direct_rel32_call_rvas.push_back(
                            section.rva + static_cast<uint32_t>(i));
            }
        }
        for (auto& e : report.symbols)
            deduplicate(e.direct_rel32_call_rvas);
        return true;
    } catch (...) {
        return false;
    }
}

bool probe_xg_callsites(const scarlett::PeFile& file,
                        XgCallsiteReport& report) noexcept {
    try {
        XgCallsiteReport found;
        found.image_base = file.details().preferred_base;
        for (const auto& module : file.details().imports) {
            if (!matches_ascii_no_case(module.name, "xg_xs.dll")) continue;
            for (const auto& symbol : module.symbols) {
                if (symbol.by_ordinal) continue;
                for (auto& e : found.symbols) {
                    if (!matches_ascii_no_case(symbol.name, e.symbol)) continue;
                    if (e.imported && e.iat_rva != symbol.iat_rva)
                        return false; // ambiguous duplicate import
                    e.imported = true;
                    e.iat_rva = symbol.iat_rva;
                }
            }
        }

        std::vector<ExecutableBytes> sections;
        for (const auto& section : file.details().sections) {
            if (!(section.characteristics & IMAGE_SCN_MEM_EXECUTE) ||
                !section.file_size) continue;
            const auto* raw = file.raw_rva(
                section.rva, section.file_size);
            sections.push_back({
                section.rva, std::span<const uint8_t>(
                    raw, std::min<size_t>(section.file_size,
                                          section.virtual_size))
            });
        }
        if (!scan_xg_executable_sections(sections, found)) return false;
        report = std::move(found);
        return true;
    } catch (...) {
        return false;
    }
}


bool gather_xg_call_contexts(
    std::span<const ExecutableBytes> sections,
    const XgCallsiteReport& report,
    std::vector<XgCallContext>& contexts,
    size_t before_bytes, size_t after_bytes) noexcept {
    try {
        // Deliberately limit data retained per callsite; callers may inspect
        // only real executable section bytes, never adjacent PE file padding.
        if (before_bytes > 4096 || after_bytes > 4096) return false;
        std::vector<XgCallContext> found;
        for (const auto& symbol : report.symbols) {
            for (uint32_t call_rva : symbol.direct_rel32_call_rvas) {
                size_t matches = 0;
                XgCallContext candidate;
                for (const auto& section : sections) {
                    if (section.bytes.size() > UINT32_MAX ||
                        uint64_t{section.rva} + section.bytes.size() > UINT32_MAX)
                        return false;
                    if (call_rva < section.rva) continue;
                    const uint64_t offset = uint64_t{call_rva} - section.rva;
                    if (offset > section.bytes.size() ||
                        section.bytes.size() - static_cast<size_t>(offset) < 5)
                        continue;
                    const size_t at = static_cast<size_t>(offset);
                    const auto raw = section.bytes;
                    if (raw[at] != 0xE8) return false;
                    uint32_t target = 0;
                    if (!rip_target(call_rva, 5,
                                    read_disp32(raw.data() + at + 1), target) ||
                        std::find(symbol.trampoline_rvas.begin(),
                                  symbol.trampoline_rvas.end(), target) ==
                            symbol.trampoline_rvas.end())
                        return false;
                    ++matches;
                    candidate.symbol = symbol.symbol;
                    candidate.call_rva = call_rva;
                    candidate.target_trampoline_rva = target;
                    candidate.section_rva = section.rva;
                    const size_t pre = std::min(at, before_bytes);
                    const size_t post = std::min(raw.size() - at - 5,
                                                 after_bytes);
                    candidate.before.assign(raw.begin() + (at - pre),
                                            raw.begin() + at);
                    std::copy_n(raw.begin() + at, 5,
                                candidate.call_bytes.begin());
                    candidate.after.assign(raw.begin() + at + 5,
                                           raw.begin() + at + 5 + post);
                }
                if (matches != 1) return false; // missing or overlapping sections
                found.push_back(std::move(candidate));
            }
        }
        std::sort(found.begin(), found.end(),
                  [](const XgCallContext& a, const XgCallContext& b) {
                      return a.call_rva < b.call_rva;
                  });
        contexts = std::move(found); // all-or-nothing result
        return true;
    } catch (...) {
        return false;
    }
}

bool probe_xg_call_contexts(
    const scarlett::PeFile& file,
    const XgCallsiteReport& report,
    std::vector<XgCallContext>& contexts,
    size_t before_bytes,
    size_t after_bytes) noexcept {
    try {
        std::vector<ExecutableBytes> sections;
        for (const auto& section : file.details().sections) {
            if (!(section.characteristics & IMAGE_SCN_MEM_EXECUTE) ||
                !section.file_size) continue;
            const auto* raw = file.raw_rva(section.rva, section.file_size);
            sections.push_back({
                section.rva, std::span<const uint8_t>(
                    raw, std::min<size_t>(section.file_size,
                                          section.virtual_size))
            });
        }
        return gather_xg_call_contexts(
            sections, report, contexts, before_bytes, after_bytes);
    } catch (...) {
        return false;
    }
}

bool verify_xg_context_fixtures() noexcept {
    std::array<uint8_t, 96> bytes{};
    bytes.fill(0x90); // generated test code, never title bytes
    constexpr uint32_t section_rva = 0x400;
    constexpr uint32_t thunk_rva = 0x450;
    for (size_t at : {size_t{8}, size_t{87}}) {
        bytes[at] = 0xE8;
        const int32_t displacement = static_cast<int32_t>(
            int64_t{thunk_rva} - (section_rva + at + 5));
        std::memcpy(bytes.data() + at + 1, &displacement,
                    sizeof(displacement));
    }
    XgCallsiteReport report;
    report.symbols[0].imported = true;
    report.symbols[0].trampoline_rvas = {thunk_rva};
    report.symbols[0].direct_rel32_call_rvas =
        {section_rva + 8, section_rva + 87};
    const std::array<ExecutableBytes, 1> sections{{
        {section_rva, std::span<const uint8_t>(bytes)}
    }};
    std::vector<XgCallContext> contexts;
    if (!gather_xg_call_contexts(sections, report, contexts, 24, 24) ||
        contexts.size() != 2 ||
        contexts[0].call_rva != section_rva + 8 ||
        contexts[0].target_trampoline_rva != thunk_rva ||
        contexts[0].before.size() != 8 ||
        contexts[0].after.size() != 24 ||
        contexts[1].before.size() != 24 ||
        contexts[1].after.size() != 4 ||
        contexts[1].call_bytes[0] != 0xE8)
        return false;

    // A failed attempt must not leave partially extracted windows behind.
    const size_t expected_size = contexts.size();
    bytes[87] = 0x90;
    if (gather_xg_call_contexts(sections, report, contexts, 24, 24) ||
        contexts.size() != expected_size) return false;
    bytes[87] = 0xE8;

    // A report whose call target is different from the actual rel32 is
    // rejected instead of trusting its unverified claimed thunk.
    report.symbols[0].trampoline_rvas = {thunk_rva + 1};
    if (gather_xg_call_contexts(sections, report, contexts)) return false;
    report.symbols[0].trampoline_rvas = {thunk_rva};

    // A truncated five-byte call, overlapping sections, and oversized
    // context requests are each rejected.
    report.symbols[0].direct_rel32_call_rvas = {section_rva + 93};
    if (gather_xg_call_contexts(sections, report, contexts)) return false;
    report.symbols[0].direct_rel32_call_rvas = {section_rva + 8};
    const std::array<ExecutableBytes, 2> overlap{{
        sections[0], sections[0]
    }};
    if (gather_xg_call_contexts(overlap, report, contexts) ||
        gather_xg_call_contexts(sections, report, contexts, 4097, 20))
        return false;
    return true;
}

bool verify_xg_callsite_fixtures() noexcept {
    // Generated x64 byte sequences: two nonadjacent import slots, two
    // RIP-relative jump stubs and three relative call instruction patterns.
    std::array<uint8_t, 160> buffer{};
    buffer.fill(0x90);
    constexpr uint32_t section_rva = 0x300;
    constexpr uint32_t iat_a = 0x2900, iat_b = 0x2910;
    auto displacement = [&](size_t offset, uint8_t opcode,
                            uint8_t modrm, uint32_t target) {
        buffer[offset] = opcode;
        const size_t size = opcode == 0xFF ? 6 : 5;
        if (opcode == 0xFF) buffer[offset + 1] = modrm;
        const int64_t signed_disp =
            int64_t{target} - (section_rva + offset + size);
        if (signed_disp < INT32_MIN || signed_disp > INT32_MAX) return false;
        const int32_t disp = static_cast<int32_t>(signed_disp);
        std::memcpy(buffer.data() + offset + (opcode == 0xFF ? 2 : 1),
                    &disp, sizeof(disp));
        return true;
    };
    if (!displacement(8, 0xFF, 0x25, iat_a) ||
        !displacement(24, 0xFF, 0x25, iat_b) ||
        !displacement(52, 0xE8, 0, section_rva + 8) ||
        !displacement(80, 0xE8, 0, section_rva + 8) ||
        !displacement(110, 0xE8, 0, section_rva + 24))
        return false;
    XgCallsiteReport evidence;
    evidence.symbols[0].imported = true;
    evidence.symbols[0].iat_rva = iat_a;
    evidence.symbols[1].imported = true;
    evidence.symbols[1].iat_rva = iat_b;
    std::array<ExecutableBytes, 1> regions{{
        {section_rva, std::span<const uint8_t>(buffer)}
    }};
    if (!scan_xg_executable_sections(regions, evidence) ||
        evidence.executable_bytes_scanned != buffer.size() ||
        evidence.symbols[0].trampoline_rvas !=
            std::vector<uint32_t>{section_rva + 8} ||
        evidence.symbols[1].trampoline_rvas !=
            std::vector<uint32_t>{section_rva + 24} ||
        evidence.symbols[0].direct_rel32_call_rvas !=
            std::vector<uint32_t>{section_rva + 52, section_rva + 80} ||
        evidence.symbols[1].direct_rel32_call_rvas !=
            std::vector<uint32_t>{section_rva + 110})
        return false;

    // The same bytes with one wrong RIP displacement must not claim a
    // corresponding callsite through a no-longer-matching IAT stub.
    buffer[8 + 2] ^= 0x40;
    if (!scan_xg_executable_sections(regions, evidence) ||
        !evidence.symbols[0].trampoline_rvas.empty() ||
        !evidence.symbols[0].direct_rel32_call_rvas.empty() ||
        evidence.symbols[1].trampoline_rvas.size() != 1 ||
        evidence.symbols[1].direct_rel32_call_rvas.size() != 1)
        return false;
    // No wrapping arithmetic or out-of-bounds section reads.
    const std::array<ExecutableBytes,1> invalid{{
        {UINT32_MAX - 1, std::span<const uint8_t>(buffer)}
    }};
    if (scan_xg_executable_sections(invalid, evidence)) return false;
    return true;
}
} // namespace scarlett::analysis
