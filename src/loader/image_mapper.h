#pragma once
#include "../pe/pe_file.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace scarlett {
class MappedImage final {
public:
    explicit MappedImage(const PeFile& pe);
    ~MappedImage();
    MappedImage(const MappedImage&) = delete;
    MappedImage& operator=(const MappedImage&) = delete;
    uint64_t base_address() const noexcept { return reinterpret_cast<uint64_t>(base_); }
    size_t applied_relocations() const noexcept { return relocations_; }
    uint32_t entry_rva() const noexcept { return entry_rva_; }
private:
    uint8_t* base_ = nullptr;
    size_t size_ = 0;
    size_t relocations_ = 0;
    uint32_t entry_rva_ = 0;
};
} // namespace scarlett
