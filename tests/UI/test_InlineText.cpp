#include "UI/InlineText.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace UI
{
namespace
{

TEST(InlineTextTest, DefaultIsEmpty)
{
    const InlineText text;
    EXPECT_TRUE(text.empty());
    EXPECT_EQ(text.size(), 0U);
    EXPECT_EQ(text.view(), "");
}

TEST(InlineTextTest, HoldsAStringLiteral)
{
    const InlineText text = "Fan: unavailable this sample";
    EXPECT_FALSE(text.empty());
    EXPECT_EQ(text.view(), "Fan: unavailable this sample");
    EXPECT_TRUE(text == "Fan: unavailable this sample");
}

TEST(InlineTextTest, FormatsLikeStdFormat)
{
    const auto text = InlineText::format("{}: {:.0f} MHz ({} of {:.0f} MHz)", "Clock", 1234.4, "62%", 2000.0);
    EXPECT_EQ(text.view(), "Clock: 1234 MHz (62% of 2000 MHz)");
}

TEST(InlineTextTest, LongTextIsCutAtCapacity)
{
    const std::string longText(InlineText::CAPACITY + 50, 'x');
    const auto formatted = InlineText::format("{}", longText);
    EXPECT_EQ(formatted.size(), InlineText::CAPACITY);
    EXPECT_EQ(formatted.view(), std::string_view(longText).substr(0, InlineText::CAPACITY));

    const InlineText constructed(std::string_view{longText});
    EXPECT_EQ(constructed.size(), InlineText::CAPACITY);
}

TEST(InlineTextTest, ACutNeverSplitsAUtf8Character)
{
    // "°" is two bytes (C2 B0). Put its first byte at the last slot, so a byte cut would keep half of it.
    const std::string text = std::string(InlineText::CAPACITY - 1, 'a') + "°C";
    const auto formatted = InlineText::format("{}", text);
    EXPECT_EQ(formatted.size(), InlineText::CAPACITY - 1);
    EXPECT_EQ(formatted.view(), std::string(InlineText::CAPACITY - 1, 'a'));
}

TEST(InlineTextTest, Utf8PrefixLengthBacksOffToACharacterBoundary)
{
    EXPECT_EQ(utf8PrefixLength("abc", 10), 3U);
    EXPECT_EQ(utf8PrefixLength("abc", 2), 2U);
    // "a°" = 61 C2 B0: cutting at 2 would split "°".
    EXPECT_EQ(utf8PrefixLength("a°", 2), 1U);
    EXPECT_EQ(utf8PrefixLength("a°b", 3), 3U);
    // A three-byte character (E2 82 AC, "€") cut after one or two of its bytes.
    EXPECT_EQ(utf8PrefixLength("€!", 1), 0U);
    EXPECT_EQ(utf8PrefixLength("€!", 2), 0U);
    EXPECT_EQ(utf8PrefixLength("€!", 3), 3U);
}

} // namespace
} // namespace UI
