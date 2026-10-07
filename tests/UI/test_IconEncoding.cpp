/// @file test_IconEncoding.cpp
/// @brief Checks that the icon macros in UI/IconsFontAwesome6.h encode the code points they claim.
///
/// The macros are hand-written UTF-8 byte strings with the code point in a trailing comment, and
/// nothing ties the two together. ICON_FA_ETHERNET was written as the bytes for U+F6D6 beside a
/// comment saying U+F796; U+F6D6 is not in the bundled font, so every Ethernet adapter in the
/// Interface Status table showed the fallback "?" instead of its icon.

#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace UI
{
namespace
{

/// Decodes the single UTF-8 sequence that makes up `utf8`. Returns 0 if `utf8` is not exactly one
/// well-formed sequence of one to three bytes, which is all the icon range (U+E005..U+F8FF) and the
/// two ASCII icons need.
[[nodiscard]] constexpr std::uint32_t decodeSingleCodePoint(std::string_view utf8) noexcept
{
    const auto byte = [utf8](std::size_t i)
    {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i]));
    };
    const auto isContinuation = [&byte](std::size_t i)
    {
        return (byte(i) & 0xC0U) == 0x80U;
    };

    if (utf8.size() == 1 && byte(0) < 0x80U)
    {
        return byte(0);
    }
    if (utf8.size() == 2 && (byte(0) & 0xE0U) == 0xC0U && isContinuation(1))
    {
        return ((byte(0) & 0x1FU) << 6U) | (byte(1) & 0x3FU);
    }
    if (utf8.size() == 3 && (byte(0) & 0xF0U) == 0xE0U && isContinuation(1) && isContinuation(2))
    {
        return ((byte(0) & 0x0FU) << 12U) | ((byte(1) & 0x3FU) << 6U) | (byte(2) & 0x3FU);
    }
    return 0;
}

// The decoder itself, against values worked out by hand.
static_assert(decodeSingleCodePoint("+") == 0x2BU);
static_assert(decodeSingleCodePoint("\xef\x80\x8d") == 0xF00DU);
static_assert(decodeSingleCodePoint("\xef\x9e\x96") == 0xF796U);
static_assert(decodeSingleCodePoint("\xef\x9b\x96") == 0xF6D6U); // what ICON_FA_ETHERNET used to be
static_assert(decodeSingleCodePoint("\xef\x80") == 0U);          // truncated
static_assert(decodeSingleCodePoint("") == 0U);

// The two that were wrong.
TEST(IconEncodingTest, EthernetIsTheEthernetGlyph)
{
    EXPECT_EQ(decodeSingleCodePoint(ICON_FA_ETHERNET), 0xF796U);
}

TEST(IconEncodingTest, DoorOpenIsTheDoorOpenGlyph)
{
    EXPECT_EQ(decodeSingleCodePoint(ICON_FA_DOOR_OPEN), 0xF52BU);
}

// The icons the Interface Status table and the title bar depend on. TitleBarLayer hard-codes the
// code points of two of them to look their glyphs up in the baked font, so those must stay in step.
TEST(IconEncodingTest, IconsUsedByCodePointMatchTheirMacros)
{
    EXPECT_EQ(decodeSingleCodePoint(ICON_FA_XMARK), 0xF00DU);
    EXPECT_EQ(decodeSingleCodePoint(ICON_FA_WINDOW_MINIMIZE), 0xF2D1U);
    EXPECT_EQ(decodeSingleCodePoint(ICON_FA_NETWORK_WIRED), 0xF6FFU);
    EXPECT_EQ(decodeSingleCodePoint(ICON_FA_WIFI), 0xF1EBU);
    EXPECT_EQ(decodeSingleCodePoint(ICON_FA_HOUSE), 0xF015U);
}

// Every icon must fall in the range the font atlas is asked to bake, or it renders as "?".
TEST(IconEncodingTest, NetworkIconsAreInsideTheBakedRange)
{
    for (const char* icon : {ICON_FA_NETWORK_WIRED, ICON_FA_WIFI, ICON_FA_ETHERNET, ICON_FA_HOUSE, ICON_FA_DOOR_OPEN})
    {
        const std::uint32_t codePoint = decodeSingleCodePoint(icon);
        EXPECT_GE(codePoint, ICON_MIN_FA);
        EXPECT_LE(codePoint, ICON_MAX_FA);
    }
}

// The Processes table's tree carets and toolbar icons (#1209).
TEST(IconEncodingTest, ProcessTableIconsAreTheirGlyphsInsideTheBakedRange)
{
    EXPECT_EQ(decodeSingleCodePoint(ICON_FA_CARET_RIGHT), 0xF0DAU);
    EXPECT_EQ(decodeSingleCodePoint(ICON_FA_CARET_DOWN), 0xF0D7U);
    EXPECT_EQ(decodeSingleCodePoint(ICON_FA_TABLE_COLUMNS), 0xF0DBU);
    EXPECT_EQ(decodeSingleCodePoint(ICON_FA_SITEMAP), 0xF0E8U);
    EXPECT_EQ(decodeSingleCodePoint(ICON_FA_ROTATE_LEFT), 0xF2EAU);
    for (const char* icon : {ICON_FA_CARET_RIGHT, ICON_FA_CARET_DOWN, ICON_FA_TABLE_COLUMNS, ICON_FA_SITEMAP, ICON_FA_ROTATE_LEFT})
    {
        const std::uint32_t codePoint = decodeSingleCodePoint(icon);
        EXPECT_GE(codePoint, ICON_MIN_FA);
        EXPECT_LE(codePoint, ICON_MAX_FA);
    }
}

} // namespace
} // namespace UI
