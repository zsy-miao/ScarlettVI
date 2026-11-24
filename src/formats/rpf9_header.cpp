#include "rpf9_header.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace scarlett::formats {
namespace {
uint32_t le32(const std::array<unsigned char, 64>& bytes, size_t offset) {
    if (offset + 4 > bytes.size()) throw std::runtime_error("Invalid header offset");
    // Endianness is explicit: this does not depend on the host's alignment.
    return uint32_t(bytes[offset]) |
           (uint32_t(bytes[offset + 1]) << 8) |
           (uint32_t(bytes[offset + 2]) << 16) |
           (uint32_t(bytes[offset + 3]) << 24);
}
std::string extension(const std::filesystem::path& path) {
    std::string e = path.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return e;
}
}

Rpf9Header read_rpf9_header(const std::filesystem::path& path) {
    Rpf9Header header;
    header.path = path;
    header.archive_bytes = std::filesystem::file_size(path);
    if (header.archive_bytes < 64) throw std::runtime_error("RPF9 archive header is truncated");
    std::ifstream file(path, std::ios::binary);
    std::array<unsigned char, 64> bytes{};
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Failed reading RPF9 archive header");
    if (le32(bytes, 0) != 0x52504639) // bytes "39 46 50 52" on disk
        throw std::runtime_error("Not an RPF9 archive (expected 9FPR magic)");
    header.toc_end = le32(bytes, 0x04);
    header.unknown_08 = le32(bytes, 0x08);
    header.length_field = le32(bytes, 0x0C);
    header.entry_count = le32(bytes, 0x24);
    header.index_variant = le32(bytes, 0x2C);
    if (header.toc_end < bytes.size() || header.toc_end > header.archive_bytes)
        throw std::runtime_error("RPF9 table boundary is outside archive file");
    if (header.entry_count > 10000000)
        throw std::runtime_error("RPF9 entry count is implausibly large");
    // Only the header is read. Payload, encrypted TOC and proprietary game data
    // are never copied, extracted, or modified by this inspector.
    return header;
}

std::vector<Rpf9Header> enumerate_rpf9(const std::filesystem::path& folder) {
    std::vector<Rpf9Header> result;
    if (std::filesystem::is_regular_file(folder)) {
        result.push_back(read_rpf9_header(folder));
        return result;
    }
    if (!std::filesystem::is_directory(folder))
        throw std::runtime_error("Archive path must be a directory or RPF9 file");
    // Top-level only: a package may contain hundreds of GB of nested archives.
    // We intentionally avoid a recursive scan or opening archive payload data.
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.is_regular_file() && extension(entry.path()) == ".rpf")
            result.push_back(read_rpf9_header(entry.path()));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.path.filename().wstring() < b.path.filename().wstring();
    });
    return result;
}
} // namespace scarlett::formats
