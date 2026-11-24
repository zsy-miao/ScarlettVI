#include "pe_file.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace scarlett {
namespace {
template<class T> const T* offset_ptr(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        throw std::runtime_error("Truncated or invalid PE data");
    return reinterpret_cast<const T*>(bytes.data() + offset);
}
uint32_t bounded32(uint64_t value) {
    if (value > UINT32_MAX) throw std::runtime_error("PE RVA exceeds 32-bit range");
    return static_cast<uint32_t>(value);
}
struct DelayImportDescriptor {
    uint32_t attributes, name, handle, iat, lookup, bound, unload, timestamp;
};
}

PeFile::PeFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open title executable");
    const auto length = input.tellg();
    if (length < 0 || static_cast<uint64_t>(length) > (1ull << 31))
        throw std::runtime_error("Unexpected PE executable size");
    data_.resize(static_cast<size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(data_.data()), static_cast<std::streamsize>(data_.size())))
        throw std::runtime_error("Cannot read complete PE executable");
    parse();
}

const uint8_t* PeFile::raw_rva(uint32_t rva, size_t len) const {
    if (rva < details_.headers_size) {
        if (uint64_t(rva) + len <= data_.size() && uint64_t(rva) + len <= details_.headers_size)
            return data_.data() + rva;
        throw std::runtime_error("Truncated PE header RVA");
    }
    for (const auto& section : details_.sections) {
        if (rva >= section.rva &&
            uint64_t(rva) - section.rva + len <= section.file_size &&
            uint64_t(section.file_offset) + (rva - section.rva) + len <= data_.size())
            return data_.data() + section.file_offset + (rva - section.rva);
    }
    throw std::runtime_error("Invalid/unbacked PE file RVA");
}

const char* PeFile::string_rva(uint32_t rva) const {
    const char* start = reinterpret_cast<const char*>(raw_rva(rva));
    // Bounded lookup: do not let an untrusted image read beyond its mapped section.
    for (size_t i = 0; i < 1024; ++i)
        if (*reinterpret_cast<const char*>(raw_rva(bounded32(uint64_t(rva) + i))) == '\0')
            return start;
    throw std::runtime_error("Unterminated PE string");
}

void PeFile::parse() {
    const auto dos = offset_ptr<IMAGE_DOS_HEADER>(data_, 0);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
        throw std::runtime_error("Not an MZ executable");
    nt_ = offset_ptr<IMAGE_NT_HEADERS64>(data_, static_cast<size_t>(dos->e_lfanew));
    if (nt_->Signature != IMAGE_NT_SIGNATURE ||
        nt_->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt_->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        throw std::runtime_error("Only PE32+ x64 files are supported");
    if (nt_->FileHeader.NumberOfSections == 0 || nt_->FileHeader.NumberOfSections > 96 ||
        nt_->OptionalHeader.SizeOfImage == 0 || nt_->OptionalHeader.SizeOfImage > 0x80000000 ||
        nt_->OptionalHeader.SizeOfHeaders > data_.size())
        throw std::runtime_error("Invalid PE image dimensions");
    const auto& opt = nt_->OptionalHeader;
    details_.preferred_base = opt.ImageBase;
    details_.image_size = opt.SizeOfImage;
    details_.headers_size = opt.SizeOfHeaders;
    details_.entry_rva = opt.AddressOfEntryPoint;
    details_.section_alignment = opt.SectionAlignment;
    details_.machine = nt_->FileHeader.Machine;
    details_.subsystem = opt.Subsystem;
    auto directory = [&](size_t index) -> IMAGE_DATA_DIRECTORY {
        return index < opt.NumberOfRvaAndSizes ? opt.DataDirectory[index] : IMAGE_DATA_DIRECTORY{};
    };
    const auto relocation = directory(IMAGE_DIRECTORY_ENTRY_BASERELOC);
    details_.relocation_rva = relocation.VirtualAddress;
    details_.relocation_size = relocation.Size;
    details_.tls_rva = directory(IMAGE_DIRECTORY_ENTRY_TLS).VirtualAddress;
    const auto exceptions = directory(IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    details_.exception_rva = exceptions.VirtualAddress;
    details_.exception_size = exceptions.Size;
    const size_t sections_start = static_cast<size_t>(dos->e_lfanew) +
        offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt_->FileHeader.SizeOfOptionalHeader;
    if (sections_start > data_.size() ||
        uint64_t(sections_start) + uint64_t(nt_->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER) > data_.size())
        throw std::runtime_error("Truncated PE section table");
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(data_.data() + sections_start);
    for (uint16_t i = 0; i < nt_->FileHeader.NumberOfSections; ++i) {
        const auto& s = sections[i];
        SectionInfo desc;
        desc.name.assign(reinterpret_cast<const char*>(s.Name), strnlen(reinterpret_cast<const char*>(s.Name), IMAGE_SIZEOF_SHORT_NAME));
        desc.rva = s.VirtualAddress;
        desc.virtual_size = s.Misc.VirtualSize;
        desc.file_offset = s.PointerToRawData;
        desc.file_size = s.SizeOfRawData;
        desc.characteristics = s.Characteristics;
        if (uint64_t(desc.rva) + std::max(desc.virtual_size, desc.file_size) > details_.image_size ||
            (desc.file_size && uint64_t(desc.file_offset) + desc.file_size > data_.size()))
            throw std::runtime_error("PE section outside file/image boundaries");
        details_.sections.emplace_back(std::move(desc));
    }
    parse_imports();
    parse_delay_imports();
    parse_exports();
}

std::vector<ImportedSymbol> PeFile::parse_thunks(uint32_t lookup_rva, uint32_t iat_rva) const {
    std::vector<ImportedSymbol> symbols;
    if (!lookup_rva || !iat_rva) return symbols;
    for (uint32_t i = 0; i < 100000; ++i) {
        const uint32_t lookup = bounded32(uint64_t(lookup_rva) + uint64_t(i) * sizeof(uint64_t));
        const uint64_t thunk = *reinterpret_cast<const uint64_t*>(raw_rva(lookup, sizeof(uint64_t)));
        if (!thunk) return symbols;
        ImportedSymbol symbol;
        symbol.iat_rva = bounded32(uint64_t(iat_rva) + uint64_t(i) * sizeof(uint64_t));
        if (thunk & IMAGE_ORDINAL_FLAG64) {
            symbol.by_ordinal = true;
            symbol.ordinal = static_cast<uint16_t>(thunk & 0xffffu);
        } else {
            const auto hint_rva = bounded32(thunk);
            raw_rva(hint_rva, sizeof(uint16_t));
            symbol.name = string_rva(bounded32(uint64_t(hint_rva) + sizeof(uint16_t)));
        }
        symbols.push_back(std::move(symbol));
    }
    throw std::runtime_error("PE import thunk limit exceeded");
}

void PeFile::parse_exports() {
    const auto& opt = nt_->OptionalHeader;
    if (opt.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return;
    const auto dir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) return;
    const auto export_header = *reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        raw_rva(dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)));
    if (export_header.NumberOfFunctions > 1000000 || export_header.NumberOfNames > export_header.NumberOfFunctions)
        throw std::runtime_error("Implausible PE export table size");
    if (!export_header.NumberOfFunctions) return;
    const auto* functions = reinterpret_cast<const uint32_t*>(
        raw_rva(export_header.AddressOfFunctions, size_t(export_header.NumberOfFunctions) * sizeof(uint32_t)));
    const uint32_t* names = nullptr;
    const uint16_t* indexes = nullptr;
    if (export_header.NumberOfNames) {
        names = reinterpret_cast<const uint32_t*>(
            raw_rva(export_header.AddressOfNames, size_t(export_header.NumberOfNames) * sizeof(uint32_t)));
        indexes = reinterpret_cast<const uint16_t*>(
            raw_rva(export_header.AddressOfNameOrdinals, size_t(export_header.NumberOfNames) * sizeof(uint16_t)));
    }
    details_.exports.reserve(export_header.NumberOfFunctions);
    for (uint32_t i = 0; i < export_header.NumberOfFunctions; ++i) {
        ExportedSymbol symbol;
        symbol.ordinal = export_header.Base + i;
        symbol.rva = functions[i];
        if (symbol.rva && symbol.rva >= dir.VirtualAddress &&
            uint64_t(symbol.rva) < uint64_t(dir.VirtualAddress) + dir.Size)
            symbol.forwarder = string_rva(symbol.rva);
        details_.exports.push_back(std::move(symbol));
    }
    for (uint32_t i = 0; i < export_header.NumberOfNames; ++i) {
        if (indexes[i] >= details_.exports.size())
            throw std::runtime_error("PE export ordinal index out of bounds");
        details_.exports[indexes[i]].name = string_rva(names[i]);
    }
}

void PeFile::parse_imports() {
    const auto& opt = nt_->OptionalHeader;
    if (opt.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return;
    const auto dir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return;
    for (uint32_t i = 0; i < 4096; ++i) {
        const auto rva = bounded32(uint64_t(dir.VirtualAddress) + uint64_t(i) * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        if (uint64_t(i + 1) * sizeof(IMAGE_IMPORT_DESCRIPTOR) > dir.Size)
            throw std::runtime_error("Unterminated PE import table");
        const auto entry = *reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(raw_rva(rva, sizeof(IMAGE_IMPORT_DESCRIPTOR)));
        if (!entry.Name && !entry.FirstThunk && !entry.OriginalFirstThunk) return;
        ImportedModule module;
        module.name = string_rva(entry.Name);
        module.symbols = parse_thunks(entry.OriginalFirstThunk ? entry.OriginalFirstThunk : entry.FirstThunk,
                                     entry.FirstThunk);
        details_.imports.push_back(std::move(module));
    }
    throw std::runtime_error("PE import table exceeds module limit");
}

void PeFile::parse_delay_imports() {
    const auto& opt = nt_->OptionalHeader;
    if (opt.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT) return;
    const auto dir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return;
    for (uint32_t i = 0; i < 4096; ++i) {
        if (uint64_t(i + 1) * sizeof(DelayImportDescriptor) > dir.Size) break;
        const auto rva = bounded32(uint64_t(dir.VirtualAddress) + uint64_t(i) * sizeof(DelayImportDescriptor));
        const auto entry = *reinterpret_cast<const DelayImportDescriptor*>(raw_rva(rva, sizeof(DelayImportDescriptor)));
        if (!entry.name) return;
        if ((entry.attributes & 1) == 0)
            throw std::runtime_error("Legacy VA-based delay imports are not yet supported");
        ImportedModule module;
        module.name = string_rva(entry.name);
        module.delay_loaded = true;
        module.symbols = parse_thunks(entry.lookup ? entry.lookup : entry.iat, entry.iat);
        details_.imports.push_back(std::move(module));
    }
}
} // namespace scarlett
