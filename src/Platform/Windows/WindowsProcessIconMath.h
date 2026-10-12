#pragma once

// Turning an HICON's bitmaps, as GetDIBits reads them, into straight-alpha RGBA (#1800). Pure: it
// takes the bytes, not Win32 handles, so it is tested on every platform
// (tests/Platform/WindowsMath/test_WindowsProcessIconMath.cpp) and holds no windows.h.
//
// Every bitmap is read as a 32 bpp top-down DIB, so each pixel is 4 bytes, B, G, R, X:
// - A colour icon has a colour bitmap and an AND mask of the same size. A 32 bpp icon carries its
//   own alpha in the colour bitmap's fourth byte, straight (DrawIconEx premultiplies when it draws).
//   An older icon has no alpha -- every fourth byte 0 -- and its AND mask says which pixels show:
//   a set mask bit (read as white) is transparent.
// - A monochrome icon has no colour bitmap. Its mask is twice the icon's height: the AND mask
//   above the XOR mask.

#include "Platform/IProcessIconSource.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Platform::Windows::IconMath
{

inline constexpr std::size_t BYTES_PER_PIXEL = 4;

/// Bytes in a 32 bpp image of @p width x @p height; 0 when either is not positive.
[[nodiscard]] constexpr std::size_t imageBytes(int width, int height) noexcept
{
    if (width <= 0 || height <= 0)
    {
        return 0;
    }
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * BYTES_PER_PIXEL;
}

/// Whether the 32 bpp pixel at byte offset @p offset of a mask read as a DIB is set (non-black).
[[nodiscard]] constexpr bool maskBitSet(std::span<const std::uint8_t> mask, std::size_t offset) noexcept
{
    return mask[offset] != 0U || mask[offset + 1] != 0U || mask[offset + 2] != 0U;
}

/// A colour icon's pixels from its colour bitmap (@p colorBgra) and AND mask (@p maskBgra), both
/// width x height. When no pixel of the colour bitmap has alpha, the mask supplies it; an empty or
/// short mask then makes every pixel opaque. nullopt when the colour bitmap is not width x height.
[[nodiscard]] inline std::optional<IconPixels>
iconFromColor(std::span<const std::uint8_t> colorBgra, std::span<const std::uint8_t> maskBgra, int width, int height)
{
    const std::size_t bytes = imageBytes(width, height);
    if (bytes == 0 || colorBgra.size() != bytes)
    {
        return std::nullopt;
    }

    bool hasAlpha = false;
    std::size_t offset = 3;
    while (offset < bytes)
    {
        if (colorBgra[offset] != 0U)
        {
            hasAlpha = true;
            break;
        }
        offset += BYTES_PER_PIXEL;
    }
    const bool maskUsable = maskBgra.size() >= bytes;

    IconPixels icon{.width = width, .height = height, .rgba = std::vector<std::uint8_t>(bytes)};
    std::size_t pixel = 0;
    while (pixel < bytes)
    {
        icon.rgba[pixel] = colorBgra[pixel + 2];     // R
        icon.rgba[pixel + 1] = colorBgra[pixel + 1]; // G
        icon.rgba[pixel + 2] = colorBgra[pixel];     // B
        if (hasAlpha)
        {
            icon.rgba[pixel + 3] = colorBgra[pixel + 3];
        }
        else
        {
            icon.rgba[pixel + 3] = (maskUsable && maskBitSet(maskBgra, pixel)) ? std::uint8_t{0} : std::uint8_t{255};
        }
        pixel += BYTES_PER_PIXEL;
    }
    return icon;
}

/// A monochrome icon's pixels from its mask (@p maskBgra): width x (2 * height), the AND mask above
/// the XOR mask. AND set: transparent (where XOR is also set, Windows inverts the screen, which an
/// image cannot; it is left transparent too). AND clear: XOR set is white, clear is black. nullopt
/// when the mask is not that size.
[[nodiscard]] inline std::optional<IconPixels> iconFromMonochrome(std::span<const std::uint8_t> maskBgra, int width, int height)
{
    const std::size_t bytes = imageBytes(width, height);
    if (bytes == 0 || maskBgra.size() != bytes * 2)
    {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> andMask = maskBgra.first(bytes);
    const std::span<const std::uint8_t> xorMask = maskBgra.subspan(bytes);

    IconPixels icon{.width = width, .height = height, .rgba = std::vector<std::uint8_t>(bytes)};
    std::size_t pixel = 0;
    while (pixel < bytes)
    {
        const bool transparent = maskBitSet(andMask, pixel);
        const std::uint8_t value = (!transparent && maskBitSet(xorMask, pixel)) ? std::uint8_t{255} : std::uint8_t{0};
        icon.rgba[pixel] = value;
        icon.rgba[pixel + 1] = value;
        icon.rgba[pixel + 2] = value;
        icon.rgba[pixel + 3] = transparent ? std::uint8_t{0} : std::uint8_t{255};
        pixel += BYTES_PER_PIXEL;
    }
    return icon;
}

} // namespace Platform::Windows::IconMath
