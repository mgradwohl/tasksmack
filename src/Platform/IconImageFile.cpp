#include "IconImageFile.h"

#include "Platform/IProcessIconSource.h"

// NOLINTNEXTLINE(misc-include-cleaner) - stb_image.h's declarations; the implementation is Platform/StbImage.cpp
#include <stb_image.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{

namespace
{

/// Frees what stbi_load() returned.
struct StbiFree
{
    void operator()(stbi_uc* pixels) const noexcept
    {
        stbi_image_free(pixels);
    }
};

} // namespace

std::optional<IconPixels> decodeIconImageFile(const std::string& utf8Path)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    // The header first: an oversized image is refused before any pixel is decoded.
    if (stbi_info(utf8Path.c_str(), &width, &height, &channels) == 0 || width <= 0 || height <= 0 || width > MAX_ICON_IMAGE_SIDE_PX ||
        height > MAX_ICON_IMAGE_SIDE_PX)
    {
        return std::nullopt;
    }
    const std::unique_ptr<stbi_uc, StbiFree> pixels(stbi_load(utf8Path.c_str(), &width, &height, &channels, STBI_rgb_alpha));
    if (pixels == nullptr || width <= 0 || height <= 0 || width > MAX_ICON_IMAGE_SIDE_PX || height > MAX_ICON_IMAGE_SIDE_PX)
    {
        return std::nullopt;
    }
    const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    IconPixels icon{.width = width, .height = height, .rgba = std::vector<std::uint8_t>(bytes)};
    std::copy_n(pixels.get(), bytes, icon.rgba.begin());
    return icon;
}

} // namespace Platform
