#include "image_mapper.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace scarlett {
MappedImage::MappedImage(const PeFile& pe) {
    const auto& d = pe.details();
    size_ = d.image_size;
    entry_rva_ = d.entry_rva;
    // Reserve preferred image base when it is free; otherwise relocate.
    base_ = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(d.preferred_base),
                                                size_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base_) base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base_) throw std::runtime_error("VirtualAlloc failed to reserve PE image address space");
    try {
        memcpy(base_, pe.bytes().data(), d.headers_size);
        for (const auto& s : d.sections) {
            if (!s.file_size) continue;
            memcpy(base_ + s.rva, pe.bytes().data() + s.file_offset, s.file_size);
        }
        const int64_t delta = static_cast<int64_t>(reinterpret_cast<uint64_t>(base_) - d.preferred_base);
        if (delta) {
            if (!d.relocation_rva || !d.relocation_size)
                throw std::runtime_error("Image is not relocatable, and preferred base was unavailable");
            uint64_t pos = d.relocation_rva;
            const uint64_t end = pos + d.relocation_size;
            if (end > size_) throw std::runtime_error("Relocation directory lies outside mapped PE");
            while (pos < end) {
                if (end - pos < sizeof(IMAGE_BASE_RELOCATION))
                    throw std::runtime_error("Truncated base-relocation block");
                const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(base_ + pos);
                if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) || block->SizeOfBlock > end - pos ||
                    (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) % sizeof(uint16_t))
                    throw std::runtime_error("Invalid base-relocation block size");
                const auto* entries = reinterpret_cast<const uint16_t*>(base_ + pos + sizeof(IMAGE_BASE_RELOCATION));
                const uint32_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
                for (uint32_t i = 0; i < count; ++i) {
                    const uint16_t type = entries[i] >> 12;
                    const uint32_t offset = entries[i] & 0x0fff;
                    if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
                    if (type != IMAGE_REL_BASED_DIR64)
                        throw std::runtime_error("Unsupported x64 relocation type " + std::to_string(type));
                    const uint64_t target = uint64_t(block->VirtualAddress) + offset;
                    if (target + sizeof(uint64_t) > size_)
                        throw std::runtime_error("Base relocation targets outside image");
                    auto* value = reinterpret_cast<uint64_t*>(base_ + target);
                    *value = static_cast<uint64_t>(static_cast<int64_t>(*value) + delta);
                    ++relocations_;
                }
                pos += block->SizeOfBlock;
            }
        }
        // This is intentionally a data-only mapping: title code, TLS callbacks and
        // DllMain are NEVER executed until import bridging and exception handling exist.
    } catch (...) {
        VirtualFree(base_, 0, MEM_RELEASE);
        base_ = nullptr;
        throw;
    }
}
void MappedImage::patch_iat(uint32_t rva, uint64_t absolute_address) {
    if (uint64_t(rva) + sizeof(uint64_t) > size_)
        throw std::runtime_error("Import address table slot is outside the mapped image");
    memcpy(base_ + rva, &absolute_address, sizeof(absolute_address));
}
MappedImage::~MappedImage() {
    if (base_) VirtualFree(base_, 0, MEM_RELEASE);
}
} // namespace scarlett
