#pragma once
#include <dxgiformat.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace scarlett::gpu {

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
