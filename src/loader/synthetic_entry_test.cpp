#include "synthetic_entry_test.h"
#include "image_mapper.h"
#include "../pe/pe_file.h"
#include "../runtime/import_router.h"
#include <Windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace scarlett {
namespace {
constexpr uint64_t kPreferredBase = 0x0000000180000000ull;
constexpr uint32_t kImageSize = 0x4000;
constexpr uint32_t kTextRva = 0x1000;
constexpr uint32_t kDataRva = 0x2000;
constexpr uint32_t kRelocRva = 0x3000;
using Bytes = std::array<uint8_t, 0x800>;

template<class T> void write(Bytes& bytes, size_t offset, const T& value) {
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset)
        throw std::runtime_error("Generated PE data would exceed fixture");
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void put_bytes(Bytes& bytes, size_t offset, const void* source, size_t size) {
    if (offset > bytes.size() || size > bytes.size() - offset)
        throw std::runtime_error("Generated PE bytes would exceed fixture");
    std::memcpy(bytes.data() + offset, source, size);
}

size_t file_offset(uint32_t rva) {
    if (rva >= 0x1000 && rva < 0x1200)
        return 0x200 + rva - 0x1000;
    if (rva >= 0x2000 && rva < 0x2200)
        return 0x400 + rva - 0x2000;
    if (rva >= 0x3000 && rva < 0x3200)
        return 0x600 + rva - 0x3000;
    throw std::runtime_error("Generated PE RVA outside sections");
}

template<class T> void write_rva(Bytes& bytes, uint32_t rva,
                                  const T& value) {
    write(bytes, file_offset(rva), value);
}

Bytes generate_pe() {
    Bytes bytes{};
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 0x80;
    write(bytes, 0, dos);

    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.NumberOfSections = 3;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt.FileHeader.Characteristics =
        IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
    auto& opt = nt.OptionalHeader;
    opt.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    opt.ImageBase = kPreferredBase;
    opt.AddressOfEntryPoint = kTextRva;
    opt.BaseOfCode = kTextRva;
    opt.SizeOfCode = 0x200;
    opt.SizeOfInitializedData = 0x400;
    opt.SectionAlignment = 0x1000;
    opt.FileAlignment = 0x200;
    opt.MajorOperatingSystemVersion = 10;
    opt.MajorSubsystemVersion = 6;
    opt.SizeOfImage = kImageSize;
    opt.SizeOfHeaders = 0x200;
    opt.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    opt.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;
    opt.SizeOfStackReserve = 1024 * 1024;
    opt.SizeOfStackCommit = 4096;
    opt.SizeOfHeapReserve = 1024 * 1024;
    opt.SizeOfHeapCommit = 4096;
    opt.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {0x2100, 40};
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS] =
        {0x2040, sizeof(IMAGE_TLS_DIRECTORY64)};
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] = {kRelocRva, 20};
    write(bytes, 0x80, nt);

    IMAGE_SECTION_HEADER sections[3]{};
    auto section = [](IMAGE_SECTION_HEADER& s, const char* name,
                      uint32_t rva, uint32_t raw,
                      uint32_t characteristics) {
        std::memcpy(s.Name, name, std::strlen(name));
        s.Misc.VirtualSize = 0x200;
        s.VirtualAddress = rva;
        s.SizeOfRawData = 0x200;
        s.PointerToRawData = raw;
        s.Characteristics = characteristics;
    };
    section(sections[0], ".text", kTextRva, 0x200,
            IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ);
    section(sections[1], ".data", kDataRva, 0x400,
            IMAGE_SCN_CNT_INITIALIZED_DATA |
            IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE);
    section(sections[2], ".reloc", kRelocRva, 0x600,
            IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ);
    put_bytes(bytes, 0x188, sections, sizeof(sections));

    // x64 leaf-like synthetic entry:
    // sub rsp,28h; mov rax,[rip+IAT_GetCurrentProcessId];
    // call rax; add rsp,28h; ret
    const uint8_t code[] = {
        0x48,0x83,0xEC,0x28,
        0x48,0x8B,0x05,0,0,0,0,
        0xFF,0xD0,
        0x48,0x83,0xC4,0x28,
        0xC3
    };
    put_bytes(bytes, file_offset(kTextRva), code, sizeof(code));
    const int32_t import_disp =
        static_cast<int32_t>(0x21A0 - (kTextRva + 11));
    write_rva(bytes, kTextRva + 7, import_disp);

    // Generated TLS callback would increment a known DWORD if invoked:
    // inc DWORD PTR [rip+disp32]; ret. The mapper must defer it.
    const uint8_t callback[] = {0xFF,0x05,0,0,0,0,0xC3};
    put_bytes(bytes, file_offset(0x1040), callback, sizeof(callback));
    const int32_t callback_disp = static_cast<int32_t>(0x2030 - 0x1046);
    write_rva(bytes, 0x1042, callback_disp);

    // One relocated absolute pointer for proving actual DIR64 fixups.
    write_rva(bytes, 0x2000, kPreferredBase + 0x2080);

    // TLS directory, 8 bytes of initialized template, 8 bytes zero-fill,
    // index, and one address-terminated callback array.
    IMAGE_TLS_DIRECTORY64 tls{};
    tls.StartAddressOfRawData = kPreferredBase + 0x2090;
    tls.EndAddressOfRawData = kPreferredBase + 0x2098;
    tls.AddressOfIndex = kPreferredBase + 0x20A0;
    tls.AddressOfCallBacks = kPreferredBase + 0x20C0;
    tls.SizeOfZeroFill = 8;
    write_rva(bytes, 0x2040, tls);
    const uint64_t tls_initial = 0x1122334455667788ull;
    write_rva(bytes, 0x2090, tls_initial);
    const DWORD initial_index = 0xeeeeeeeeu;
    write_rva(bytes, 0x20A0, initial_index);
    write_rva(bytes, 0x20C0, kPreferredBase + 0x1040);

    IMAGE_IMPORT_DESCRIPTOR import_desc{};
    import_desc.OriginalFirstThunk = 0x2180;
    import_desc.Name = 0x21C0;
    import_desc.FirstThunk = 0x21A0;
    write_rva(bytes, 0x2100, import_desc);
    write_rva(bytes, 0x2180, uint64_t{0x21E0});
    write_rva(bytes, 0x21A0, uint64_t{0x21E0});
    const char dll[] = "KERNEL32.dll";
    const char symbol[] = "GetCurrentProcessId";
    put_bytes(bytes, file_offset(0x21C0), dll, sizeof(dll));
    put_bytes(bytes, file_offset(0x21E2), symbol, sizeof(symbol));

    // Relocate 1 arbitrary data pointer, 4 TLS directory VAs,
    // and 1 callback array VA, all in the same data page.
    IMAGE_BASE_RELOCATION block{};
    block.VirtualAddress = kDataRva;
    block.SizeOfBlock = 20;
    write_rva(bytes, kRelocRva, block);
    const std::array<uint16_t, 6> entries{
        static_cast<uint16_t>((IMAGE_REL_BASED_DIR64 << 12) | 0x000),
        static_cast<uint16_t>((IMAGE_REL_BASED_DIR64 << 12) | 0x040),
        static_cast<uint16_t>((IMAGE_REL_BASED_DIR64 << 12) | 0x048),
        static_cast<uint16_t>((IMAGE_REL_BASED_DIR64 << 12) | 0x050),
        static_cast<uint16_t>((IMAGE_REL_BASED_DIR64 << 12) | 0x058),
        static_cast<uint16_t>((IMAGE_REL_BASED_DIR64 << 12) | 0x0C0)
    };
    put_bytes(bytes, file_offset(kRelocRva + 8),
              entries.data(), sizeof(entries));
    return bytes;
}

struct LocalFile {
    std::filesystem::path path;
    ~LocalFile() {
        if (!path.empty()) {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    }
};
struct VaReservation {
    void* pointer = nullptr;
    ~VaReservation() { if (pointer) VirtualFree(pointer, 0, MEM_RELEASE); }
};
} // namespace

bool verify_synthetic_entry_fixture() noexcept {
    try {
        const std::filesystem::path reports = L"reports";
        std::filesystem::create_directories(reports);
        LocalFile file{reports /
            (L"synthetic_entry_" + std::to_wstring(GetCurrentProcessId()) +
             L".exe")};
        if (std::filesystem::exists(file.path))
            return false; // never overwrite an existing user's file
        const Bytes bytes = generate_pe();
        {
            std::ofstream output(file.path, std::ios::binary);
            if (!output.write(reinterpret_cast<const char*>(bytes.data()),
                              bytes.size()))
                return false;
        }

        PeFile pe(file.path);
        const auto& detail = pe.details();
        if (detail.imports.size() != 1 ||
            detail.imports[0].name != "KERNEL32.dll" ||
            detail.imports[0].symbols.size() != 1 ||
            detail.imports[0].symbols[0].name != "GetCurrentProcessId" ||
            detail.entry_rva != kTextRva ||
            detail.tls_rva != 0x2040)
            return false;

        // Force the real mapper's relocation path even if its preferred
        // address would otherwise be free on this host.
        VaReservation reserved{
            VirtualAlloc(reinterpret_cast<void*>(kPreferredBase),
                         kImageSize, MEM_RESERVE, PAGE_NOACCESS)};
        MappedImage image(pe);
        if (image.base_address() == kPreferredBase ||
            image.applied_relocations() != 6)
            return false;
        const auto at = [&](uint32_t rva) {
            return reinterpret_cast<const uint8_t*>(
                image.base_address() + rva);
        };
        uint64_t relocated_value = 0;
        std::memcpy(&relocated_value, at(0x2000), sizeof(relocated_value));
        if (relocated_value != image.base_address() + 0x2080)
            return false;

        ImportRouter router;
        const auto binding = router.bind_windows(pe, image,
                                                 file.path.parent_path());
        if (binding.host_symbols_seen != 1 ||
            binding.host_symbols_bound != 1 ||
            binding.xbox_symbols_pending != 0 ||
            !binding.pending.empty())
            return false;
        uint64_t bound_import = 0;
        std::memcpy(&bound_import, at(0x21A0), sizeof(bound_import));
        const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        const FARPROC expected = kernel32
            ? GetProcAddress(kernel32, "GetCurrentProcessId") : nullptr;
        if (!expected ||
            bound_import != reinterpret_cast<uint64_t>(expected))
            return false;

        const ImageFinalizeReport finalized = image.finalize_image();
        if (finalized.sections_protected != 3 ||
            finalized.tls_slots_prepared != 1 ||
            finalized.tls_template_bytes != 16 ||
            finalized.deferred_tls_callbacks != 1 ||
            image.finalize_image().sections_protected != 3)
            return false;

        DWORD tls_index = TLS_OUT_OF_INDEXES;
        std::memcpy(&tls_index, at(0x20A0), sizeof(tls_index));
        if (tls_index == TLS_OUT_OF_INDEXES)
            return false;
        const auto* tls_bytes = static_cast<const uint8_t*>(
            TlsGetValue(tls_index));
        const uint64_t template_pattern = 0x1122334455667788ull;
        if (!tls_bytes ||
            std::memcmp(tls_bytes, &template_pattern, 8) != 0)
            return false;
        for (size_t i = 8; i < 16; ++i)
            if (tls_bytes[i] != 0)
                return false;

        auto* callback_ran =
            reinterpret_cast<const uint32_t*>(at(0x2030));
        if (*callback_ran != 0)
            return false;

        // Only this generated, fixed x64 code is executed. Never accept a
        // caller-supplied game image or execute a title/DLL initializer.
        using SyntheticEntry = DWORD (WINAPI*)();
        const auto entry = reinterpret_cast<SyntheticEntry>(
            image.base_address() + image.entry_rva());
        const DWORD result = entry();
        if (result != GetCurrentProcessId() || *callback_ran != 0)
            return false;

        bool finalized_patch_rejected = false;
        try {
            image.patch_iat(0x21A0,
                reinterpret_cast<uint64_t>(&GetCurrentProcessId));
        } catch (const std::logic_error&) {
            finalized_patch_rejected = true;
        }
        return finalized_patch_rejected;
    } catch (...) {
        return false;
    }
}
} // namespace scarlett
