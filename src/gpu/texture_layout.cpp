#include "texture_layout.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace scarlett::gpu {
namespace {
struct FormatInfo { uint32_t block_extent, block_bytes; };
constexpr FormatInfo format_info(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8_UNORM: return {1, 1};
    case DXGI_FORMAT_R8G8B8A8_UNORM: return {1, 4};
    case DXGI_FORMAT_BC1_UNORM: return {4, 8};
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC5_UNORM: return {4, 16};
    default: return {0, 0};
    }
}
// Conventional 3-bit Morton encoding, used solely as an ORIGINAL synthetic
// tiling fixture. Does not reproduce the Xbox hardware's XG swizzle.
constexpr uint32_t morton8(uint32_t x, uint32_t y) {
    return (x & 1u) | ((y & 1u) << 1) |
           ((x & 2u) << 1) | ((y & 2u) << 2) |
           ((x & 4u) << 2) | ((y & 4u) << 3);
}
constexpr uint64_t tile_bytes(const TextureSubresource& s) {
    const uint64_t tx = (uint64_t{s.block_width} + 7) / 8;
    const uint64_t ty = (uint64_t{s.block_height} + 7) / 8;
    return tx * ty * 64ull * s.bytes_per_block;
}
uint64_t block_offset(uint32_t x, uint32_t y, const TextureSubresource& s) {
    const uint64_t tiles_per_row = (uint64_t{s.block_width} + 7) / 8;
    const uint64_t tile_index = (y / 8) * tiles_per_row + (x / 8);
    return s.tiled_offset + (tile_index * 64 + morton8(x % 8, y % 8)) *
                              s.bytes_per_block;
}
}
std::optional<TextureLayout> TextureLayout::build(
    const TextureDescription& description) noexcept {
    constexpr uint64_t kMaxBytes = 256ull * 1024 * 1024;
    const FormatInfo format = format_info(description.format);
    if (!format.block_bytes || !description.width || !description.height ||
        description.width > 16384 || description.height > 16384 ||
        !description.array_layers || description.array_layers > 16 ||
        !description.mip_levels || description.mip_levels > 15 ||
        uint64_t{description.array_layers} * description.mip_levels > 128)
        return std::nullopt;

    // Cannot request extra empty mips past the final 1x1 level.
    uint32_t max_dimension = std::max(description.width, description.height);
    uint32_t maximum_mips = 1;
    while (max_dimension > 1) { max_dimension >>= 1; ++maximum_mips; }
    if (description.mip_levels > maximum_mips) return std::nullopt;

    TextureLayout result;
    result.description_ = description;
    try {
        result.subresources_.reserve(
            static_cast<size_t>(description.array_layers) * description.mip_levels);
        for (uint32_t layer = 0; layer < description.array_layers; ++layer) {
            uint32_t w = description.width, h = description.height;
            for (uint32_t mip = 0; mip < description.mip_levels; ++mip) {
                TextureSubresource s{};
                s.width = w;
                s.height = h;
                s.bytes_per_block = format.block_bytes;
                s.block_width = (w + format.block_extent - 1) / format.block_extent;
                s.block_height = (h + format.block_extent - 1) / format.block_extent;
                s.row_bytes = s.block_width * s.bytes_per_block;
                s.linear_bytes = uint64_t{s.row_bytes} * s.block_height;
                s.linear_offset = result.linear_size_;
                s.tiled_offset = result.tiled_size_;
                s.tiled_bytes = tile_bytes(s);
                if (s.linear_bytes > kMaxBytes - result.linear_size_ ||
                    s.tiled_bytes > kMaxBytes - result.tiled_size_)
                    return std::nullopt;
                result.linear_size_ += s.linear_bytes;
                result.tiled_size_ += s.tiled_bytes;
                result.subresources_.push_back(s);
                w = std::max(1u, w / 2);
                h = std::max(1u, h / 2);
            }
        }
    } catch (...) {
        return std::nullopt;
    }
    return result;
}

bool TextureLayout::swizzle(std::span<const uint8_t> linear,
                            std::span<uint8_t> tiled) const noexcept {
    if (linear.size() != linear_size_ || tiled.size() != tiled_size_)
        return false;
    const uintptr_t input = reinterpret_cast<uintptr_t>(linear.data());
    const uintptr_t output = reinterpret_cast<uintptr_t>(tiled.data());
    if (input <= output ? output - input < linear.size()
                        : input - output < tiled.size())
        return false; // In-place/overlapping swizzle would corrupt input.
    std::fill(tiled.begin(), tiled.end(), uint8_t{0});
    for (const auto& s : subresources_) {
        for (uint32_t y = 0; y < s.block_height; ++y)
            for (uint32_t x = 0; x < s.block_width; ++x) {
                const auto src = s.linear_offset + uint64_t{y} * s.row_bytes +
                                 uint64_t{x} * s.bytes_per_block;
                const auto dst = block_offset(x, y, s);
                std::memcpy(tiled.data() + static_cast<size_t>(dst),
                            linear.data() + static_cast<size_t>(src), s.bytes_per_block);
            }
    }
    return true;
}

bool TextureLayout::deswizzle(std::span<const uint8_t> tiled,
                              std::span<uint8_t> linear) const noexcept {
    if (linear.size() != linear_size_ || tiled.size() != tiled_size_)
        return false;
    const uintptr_t input = reinterpret_cast<uintptr_t>(tiled.data());
    const uintptr_t output = reinterpret_cast<uintptr_t>(linear.data());
    if (input <= output ? output - input < tiled.size()
                        : input - output < linear.size())
        return false; // Decode must have separate storage.
    for (const auto& s : subresources_) {
        for (uint32_t y = 0; y < s.block_height; ++y)
            for (uint32_t x = 0; x < s.block_width; ++x) {
                const auto dst = s.linear_offset + uint64_t{y} * s.row_bytes +
                                 uint64_t{x} * s.bytes_per_block;
                const auto src = block_offset(x, y, s);
                std::memcpy(linear.data() + static_cast<size_t>(dst),
                            tiled.data() + static_cast<size_t>(src), s.bytes_per_block);
            }
    }
    return true;
}

bool verify_texture_layout_fixtures(uint64_t* checked_bytes,
                                    uint32_t* checked_subresources) {
    if (checked_bytes) *checked_bytes = 0;
    if (checked_subresources) *checked_subresources = 0;
    const DXGI_FORMAT formats[]{
        DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC5_UNORM
    };
    for (const auto format : formats) {
        for (auto [width, height] :
             {std::pair{1u, 1u}, std::pair{7u, 9u},
              std::pair{53u, 37u}, std::pair{129u, 65u}}) {
            // A 1x1 texture cannot support four distinct mip levels.
            const TextureDescription request{width, height,
                width == 1 ? 1u : 4u, 2, format};
            const auto layout = TextureLayout::build(request);
            if (!layout) return false;
            std::vector<uint8_t> linear(layout->linear_size());
            std::vector<uint8_t> tiled(layout->tiled_size());
            std::vector<uint8_t> decoded(layout->linear_size(), 0xFE);
            for (size_t i = 0; i < linear.size(); ++i)
                linear[i] = static_cast<uint8_t>((i * 53 + (i >> 3) * 17 +
                                                  width * 13 + height * 7) & 255);
            if (!layout->swizzle(linear, tiled) ||
                !layout->deswizzle(tiled, decoded) || linear != decoded)
                return false;
            if (!tiled.empty() && layout->swizzle(linear, std::span<uint8_t>{}))
                return false;
            if (layout->deswizzle({}, decoded)) return false;
            std::vector<uint8_t> overlapping(layout->linear_size() +
                                              layout->tiled_size(), 0);
            if (layout->swizzle(
                    std::span(overlapping.data(), layout->linear_size()),
                    std::span(overlapping.data() + 1, layout->tiled_size())))
                return false;
            if (layout->deswizzle(
                    std::span(overlapping.data(), layout->tiled_size()),
                    std::span(overlapping.data() + 1, layout->linear_size())))
                return false;
            // Detect invalid bounds and unsupported formats.
            if (layout->subresources().size() != request.array_layers * request.mip_levels)
                return false;
            if (checked_bytes) *checked_bytes += linear.size();
            if (checked_subresources)
                *checked_subresources += static_cast<uint32_t>(layout->subresources().size());
        }
    }
    return !TextureLayout::build({0, 4, 1, 1, DXGI_FORMAT_R8_UNORM}) &&
           !TextureLayout::build({64, 64, 1, 1, DXGI_FORMAT_UNKNOWN}) &&
           !TextureLayout::build({2, 2, 15, 1, DXGI_FORMAT_BC1_UNORM}) &&
           !TextureLayout::build({16384, 16384, 15, 16, DXGI_FORMAT_R8G8B8A8_UNORM});
}
} // namespace scarlett::gpu
