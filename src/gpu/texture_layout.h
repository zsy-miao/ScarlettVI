#pragma once
#include <dxgiformat.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace scarlett::gpu {

// Host DXGI texture elements, with block-compressed elements treated as
// opaque 4x4 byte blocks. No Xbox XG format codes are implied.
struct HostTextureFormatInfo {
    uint32_t block_extent = 0;
    uint32_t block_bytes = 0;
};
HostTextureFormatInfo host_texture_format_info(DXGI_FORMAT format) noexcept;
inline constexpr std::array<DXGI_FORMAT, 12> kHostTextureFormats{
    DXGI_FORMAT_R8_UNORM,
    DXGI_FORMAT_R16_FLOAT,
    DXGI_FORMAT_R32_FLOAT,
    DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_BC1_UNORM,
    DXGI_FORMAT_BC3_UNORM,
    DXGI_FORMAT_BC4_UNORM,
    DXGI_FORMAT_BC5_UNORM,
    DXGI_FORMAT_BC6H_UF16,
    DXGI_FORMAT_BC7_UNORM,
    DXGI_FORMAT_R16G16_FLOAT
};

// Original *host* texture-layout intermediary. Microtile order is a
// deterministic 8x8 Morton address, NOT the Xbox XG swizzle algorithm.
struct TextureDescription {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 0;
    uint32_t array_layers = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
struct TextureSubresource {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t block_width = 0;
    uint32_t block_height = 0;
    uint32_t bytes_per_block = 0;
    uint32_t row_bytes = 0;
    uint64_t linear_offset = 0;
    uint64_t linear_bytes = 0;
    uint64_t tiled_offset = 0;
    uint64_t tiled_bytes = 0;
};
class TextureLayout final {
public:
    // Bounds-checks inputs; never guesses Xbox format parameters.
    static std::optional<TextureLayout> build(
        const TextureDescription& description) noexcept;
    bool swizzle(std::span<const uint8_t> linear,
                 std::span<uint8_t> tiled) const noexcept;
    bool deswizzle(std::span<const uint8_t> tiled,
                   std::span<uint8_t> linear) const noexcept;
    const TextureDescription& description() const noexcept { return description_; }
    const std::vector<TextureSubresource>& subresources() const noexcept {
        return subresources_;
    }
    uint64_t linear_size() const noexcept { return linear_size_; }
    uint64_t tiled_size() const noexcept { return tiled_size_; }
private:
    TextureDescription description_{};
    std::vector<TextureSubresource> subresources_;
    uint64_t linear_size_ = 0;
    uint64_t tiled_size_ = 0;
};

// Generated samples only; no proprietary XG swizzle patterns or title assets.
bool verify_texture_layout_fixtures(uint64_t* checked_linear_bytes = nullptr,
                                    uint32_t* checked_subresources = nullptr);
} // namespace scarlett::gpu
