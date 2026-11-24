#pragma once
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace scarlett {
struct ImportedSymbol {
    std::string name;
    uint16_t ordinal = 0;
    uint32_t iat_rva = 0;
    bool by_ordinal = false;
};
struct ImportedModule {
    std::string name;
    bool delay_loaded = false;
    std::vector<ImportedSymbol> symbols;
};
struct SectionInfo {
    std::string name;
    uint32_t rva = 0;
    uint32_t virtual_size = 0;
    uint32_t file_offset = 0;
    uint32_t file_size = 0;
    uint32_t characteristics = 0;
};
struct PeDetails {
    uint64_t preferred_base = 0;
    uint32_t image_size = 0;
    uint32_t headers_size = 0;
    uint32_t entry_rva = 0;
    uint32_t section_alignment = 0;
    uint16_t machine = 0;
    uint16_t subsystem = 0;
    uint32_t relocation_rva = 0;
    uint32_t relocation_size = 0;
    uint32_t tls_rva = 0;
    uint32_t exception_rva = 0;
    std::vector<SectionInfo> sections;
    std::vector<ImportedModule> imports;
};
class PeFile final {
public:
    explicit PeFile(const std::filesystem::path& path);
    const PeDetails& details() const noexcept { return details_; }
    const std::vector<uint8_t>& bytes() const noexcept { return data_; }
    const IMAGE_NT_HEADERS64& nt_headers() const noexcept { return *nt_; }
    const uint8_t* raw_rva(uint32_t rva, size_t byte_count = 1) const;
    const char* string_rva(uint32_t rva) const;
private:
    void parse();
    void parse_imports();
    void parse_delay_imports();
    std::vector<ImportedSymbol> parse_thunks(uint32_t int_rva, uint32_t iat_rva) const;
    std::vector<uint8_t> data_;
    const IMAGE_NT_HEADERS64* nt_ = nullptr;
    PeDetails details_;
};
} // namespace scarlett
