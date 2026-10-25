/// @file test_AsciiTrim.cpp
/// @brief Tests for Platform::trimAscii() (#2170): the one trim that replaced seven, each of which
/// stripped its own set of whitespace.

#include "Platform/AsciiTrim.h"

#include <gtest/gtest.h>

#include <string_view>

namespace
{

using Platform::trimAscii;
using namespace std::string_view_literals;

// constexpr, so HelpContent's compile-time filter and DesktopEntryIcons' parser can keep using it.
static_assert(trimAscii("  x  ") == "x");
static_assert(trimAscii("").empty());

TEST(AsciiTrimTest, StripsEveryAsciiWhitespaceCharacterFromBothEnds)
{
    EXPECT_EQ(trimAscii(" \t\n\v\f\rvalue \t\n\v\f\r"), "value");
    EXPECT_EQ(trimAscii("value\r\n"), "value") << "a line copied from a Windows file";
    EXPECT_EQ(trimAscii("\tvalue"), "value");
}

TEST(AsciiTrimTest, KeepsInnerWhitespaceAndLeavesTrimmedTextAlone)
{
    EXPECT_EQ(trimAscii("  two words  "), "two words");
    EXPECT_EQ(trimAscii("a\tb"), "a\tb");
    EXPECT_EQ(trimAscii("value"), "value");
}

TEST(AsciiTrimTest, AllWhitespaceOrEmptyGivesAnEmptyView)
{
    EXPECT_TRUE(trimAscii("").empty());
    EXPECT_TRUE(trimAscii(" \t\r\n\v\f").empty());
}

TEST(AsciiTrimTest, NonAsciiBytesAreNotWhitespace)
{
    // Unlike isspace() in some locales, a UTF-8 lead or continuation byte, or a non-breaking
    // space's bytes, is never stripped: only the six ASCII characters are.
    EXPECT_EQ(trimAscii("\xC2\xA0x\xC2\xA0"sv), "\xC2\xA0x\xC2\xA0"sv);
    EXPECT_EQ(trimAscii(" \xC3\xA9 "sv), "\xC3\xA9"sv);
}

TEST(AsciiTrimTest, TheResultIsAViewIntoTheInput)
{
    constexpr std::string_view text = "  abc  ";
    const std::string_view trimmed = trimAscii(text);
    EXPECT_EQ(trimmed.data(), text.data() + 2);
    EXPECT_EQ(trimmed.size(), 3U);
}

} // namespace
