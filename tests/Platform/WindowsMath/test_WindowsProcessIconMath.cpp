/// @file test_WindowsProcessIconMath.cpp
/// @brief An HICON's bitmaps, as GetDIBits reads them, to straight-alpha RGBA (#1800): a 32 bpp icon
/// keeps its own alpha, an icon without alpha takes it from its AND mask, a monochrome icon is built
/// from its AND and XOR masks, and wrong-sized input is refused. Pure, so it runs on every platform.

#include "Platform/IProcessIconSource.h"
#include "Platform/Windows/WindowsProcessIconMath.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace Platform::Windows::IconMath
{
namespace
{

/// One BGRX pixel.
void push(std::vector<std::uint8_t>& bytes, std::uint8_t b, std::uint8_t g, std::uint8_t r, std::uint8_t x)
{
    bytes.push_back(b);
    bytes.push_back(g);
    bytes.push_back(r);
    bytes.push_back(x);
}

TEST(WindowsProcessIconMathTest, AnIconWithAlphaKeepsItAndSwapsRedAndBlue)
{
    std::vector<std::uint8_t> color;
    push(color, 10, 20, 30, 255);
    push(color, 40, 50, 60, 128);
    push(color, 0, 0, 0, 0);
    push(color, 1, 2, 3, 7);
    // The mask says every pixel is transparent; with alpha present it is ignored.
    const std::vector<std::uint8_t> mask(16, 255U);

    const auto icon = iconFromColor(color, mask, 2, 2);
    ASSERT_TRUE(icon.has_value());
    const IconPixels loaded = icon.value_or(IconPixels{});
    EXPECT_EQ(loaded.width, 2);
    EXPECT_EQ(loaded.height, 2);
    EXPECT_EQ(loaded.rgba, (std::vector<std::uint8_t>{30, 20, 10, 255, 60, 50, 40, 128, 0, 0, 0, 0, 3, 2, 1, 7}));
}

TEST(WindowsProcessIconMathTest, AnIconWithoutAlphaTakesItFromItsMask)
{
    std::vector<std::uint8_t> color;
    push(color, 10, 20, 30, 0);
    push(color, 40, 50, 60, 0);
    std::vector<std::uint8_t> mask;
    push(mask, 0, 0, 0, 0);       // AND clear: shown
    push(mask, 255, 255, 255, 0); // AND set: transparent

    const auto icon = iconFromColor(color, mask, 2, 1);
    ASSERT_TRUE(icon.has_value());
    const IconPixels loaded = icon.value_or(IconPixels{});
    EXPECT_EQ(loaded.rgba, (std::vector<std::uint8_t>{30, 20, 10, 255, 60, 50, 40, 0}));
}

TEST(WindowsProcessIconMathTest, AnIconWithoutAlphaOrAReadableMaskIsOpaque)
{
    std::vector<std::uint8_t> color;
    push(color, 10, 20, 30, 0);
    const auto icon = iconFromColor(color, {}, 1, 1);
    ASSERT_TRUE(icon.has_value());
    const IconPixels loaded = icon.value_or(IconPixels{});
    EXPECT_EQ(loaded.rgba, (std::vector<std::uint8_t>{30, 20, 10, 255}));
}

TEST(WindowsProcessIconMathTest, AMonochromeIconIsBuiltFromItsTwoMasks)
{
    // 2x1: AND row above XOR row. Pixel 0: AND clear, XOR set -> white. Pixel 1: AND set -> transparent.
    std::vector<std::uint8_t> mask;
    push(mask, 0, 0, 0, 0);
    push(mask, 255, 255, 255, 0);
    push(mask, 255, 255, 255, 0);
    push(mask, 0, 0, 0, 0);

    const auto icon = iconFromMonochrome(mask, 2, 1);
    ASSERT_TRUE(icon.has_value());
    const IconPixels loaded = icon.value_or(IconPixels{});
    EXPECT_EQ(loaded.rgba, (std::vector<std::uint8_t>{255, 255, 255, 255, 0, 0, 0, 0}));
}

TEST(WindowsProcessIconMathTest, AMonochromeIconsClearPixelsAreBlack)
{
    const std::vector<std::uint8_t> mask(8, 0U); // 1x1: AND clear, XOR clear
    const auto icon = iconFromMonochrome(mask, 1, 1);
    ASSERT_TRUE(icon.has_value());
    const IconPixels loaded = icon.value_or(IconPixels{});
    EXPECT_EQ(loaded.rgba, (std::vector<std::uint8_t>{0, 0, 0, 255}));
}

TEST(WindowsProcessIconMathTest, WrongSizesAreRefused)
{
    const std::vector<std::uint8_t> fourPixels(16, 0U);
    EXPECT_FALSE(iconFromColor(fourPixels, {}, 3, 3).has_value());
    EXPECT_FALSE(iconFromColor(fourPixels, {}, 0, 4).has_value());
    EXPECT_FALSE(iconFromMonochrome(fourPixels, 2, 2).has_value()); // Needs 2 x 2 x 2 rows
    EXPECT_TRUE(iconFromMonochrome(fourPixels, 2, 1).has_value());
    EXPECT_EQ(imageBytes(-1, 4), 0U);
}

} // namespace
} // namespace Platform::Windows::IconMath
