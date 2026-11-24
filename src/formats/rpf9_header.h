#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace scarlett::formats {
struct Rpf9Header {
    std::filesystem::path path;
    uint64_t archive_bytes = 0;
    uint32_t toc_end = 0;
    uint32_t entry_count = 0;
    uint32_t index_variant = 0;
    uint32_t length_field = 0;
    uint32_t unknown_08 = 0;
};
Rpf9Header read_rpf9_header(const std::filesystem::path& archive);
std::vector<Rpf9Header> enumerate_rpf9(const std::filesystem::path& title_or_archive);
} // namespace scarlett::formats
