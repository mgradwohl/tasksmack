/// @file test_AsciiCase.cpp
/// @brief Tests for Platform/AsciiCase.h (#2566): the one ASCII case fold that replaced about twenty
/// hand-written contains/equals/compare-ignoring-case helpers, some of which followed the C locale.

#include "Platform/AsciiCase.h"

#include <gtest/gtest.h>

#include <compare>
#include <string_view>

namespace
{

using Platform::compareIgnoreCase;
using Platform::containsIgnoreCase;
using Platform::equalsIgnoreCase;
using Platform::foldAscii;
using Platform::upperAscii;
using namespace std::string_view_literals;

// constexpr, so HelpContent's compile-time shortcut filter and the static_asserts below can use them.
static_assert(foldAscii('A') == 'a' && foldAscii('z') == 'z' && foldAscii('1') == '1');
static_assert(upperAscii('a') == 'A' && upperAscii('Z') == 'Z' && upperAscii('-') == '-');
static_assert(equalsIgnoreCase("svchost.exe", "SVCHOST.EXE"));
static_assert(containsIgnoreCase("Microsoft Edge", "EDGE"));
static_assert(compareIgnoreCase("apple", "Banana") < 0);

TEST(AsciiCaseTest, FoldMapsOnlyTheAsciiLetters)
{
    EXPECT_EQ(foldAscii('A'), 'a');
    EXPECT_EQ(foldAscii('Z'), 'z');
    EXPECT_EQ(foldAscii('a'), 'a');
    EXPECT_EQ(foldAscii('@'), '@'); // the byte before 'A'
    EXPECT_EQ(foldAscii('['), '['); // the byte after 'Z'
    EXPECT_EQ(foldAscii('\0'), '\0');
    EXPECT_EQ(upperAscii('a'), 'A');
    EXPECT_EQ(upperAscii('z'), 'Z');
    EXPECT_EQ(upperAscii('`'), '`'); // the byte before 'a'
    EXPECT_EQ(upperAscii('{'), '{'); // the byte after 'z'
    EXPECT_EQ(upperAscii('A'), 'A');
}

TEST(AsciiCaseTest, BytesOfEightyAndUpAreNeverFolded)
{
    // UTF-8 "É" (C3 89) and "é" (C3 A9): std::toupper()/std::tolower() in a single-byte locale might
    // map these; the ASCII fold leaves every one alone, so a UTF-8 sequence never changes.
    constexpr std::string_view HIGH_BYTES = "\xC3\x89\xC3\xA9\x80\xFF"sv;
    for (const char c : HIGH_BYTES)
    {
        EXPECT_EQ(foldAscii(c), c) << static_cast<int>(static_cast<unsigned char>(c));
        EXPECT_EQ(upperAscii(c), c) << static_cast<int>(static_cast<unsigned char>(c));
    }
    EXPECT_TRUE(equalsIgnoreCase("R\xC3\xA9sum\xC3\xA9"sv, "r\xC3\xA9SUM\xC3\xA9"sv)); // the ASCII letters fold, é matches itself
    EXPECT_FALSE(equalsIgnoreCase("\xC3\x89"sv, "\xC3\xA9"sv));                        // É is not folded to é
    EXPECT_FALSE(containsIgnoreCase("R\xC3\xA9sum\xC3\xA9.exe"sv, "R\xC3\x89SUM\xC3\x89"sv));
}

TEST(AsciiCaseTest, EqualsIgnoresAsciiCaseAndNothingElse)
{
    EXPECT_TRUE(equalsIgnoreCase("", ""));
    EXPECT_TRUE(equalsIgnoreCase("svchost.exe", "SvcHost.EXE"));
    EXPECT_TRUE(equalsIgnoreCase("-k", "-K"));
    EXPECT_TRUE(equalsIgnoreCase("=C:=C:\\", "=c:=c:\\")); // a Windows environment's hidden drive entry
    EXPECT_FALSE(equalsIgnoreCase("", "a"));
    EXPECT_FALSE(equalsIgnoreCase("a", ""));
    EXPECT_FALSE(equalsIgnoreCase("svchost", "svchost.exe")) << "a prefix is not equal";
    EXPECT_FALSE(equalsIgnoreCase("svchost.exe", "svchost")) << "nor is a longer text";
    EXPECT_FALSE(equalsIgnoreCase("a_b", "a-b")) << "punctuation compares exactly";
    EXPECT_FALSE(equalsIgnoreCase("1", "l"));
}

TEST(AsciiCaseTest, EmbeddedNulIsAnOrdinaryByte)
{
    constexpr std::string_view WITH_NUL = "a\0B"sv;
    EXPECT_EQ(WITH_NUL.size(), 3U);
    EXPECT_TRUE(equalsIgnoreCase(WITH_NUL, "A\0b"sv));
    EXPECT_FALSE(equalsIgnoreCase(WITH_NUL, "a\0"sv)) << "the length counts, not strlen()";
    EXPECT_FALSE(equalsIgnoreCase(WITH_NUL, "a B"sv));
    EXPECT_TRUE(containsIgnoreCase("x\0Y\0z"sv, "\0y\0"sv));
    EXPECT_TRUE(containsIgnoreCase("x\0Y\0z"sv, "Z"sv)) << "the scan runs past the NUL";
    EXPECT_FALSE(containsIgnoreCase("x\0Y"sv, "x Y"sv));
    EXPECT_EQ(compareIgnoreCase("a\0"sv, "a\x01"sv), std::strong_ordering::less);
    EXPECT_EQ(compareIgnoreCase("A\0b"sv, "a\0B"sv), std::strong_ordering::equal);
}

TEST(AsciiCaseTest, ContainsIgnoresAsciiCaseAnywhereInTheText)
{
    EXPECT_TRUE(containsIgnoreCase("rust-analyzer", "Analyzer"));
    EXPECT_TRUE(containsIgnoreCase("rust-analyzer", "RUST"));
    EXPECT_TRUE(containsIgnoreCase("rust-analyzer", "rust-analyzer"));
    EXPECT_TRUE(containsIgnoreCase("RUST-ANALYZER", "t-a"));
    EXPECT_TRUE(containsIgnoreCase("aaab", "aab")) << "a partial match before the real one";
    EXPECT_TRUE(containsIgnoreCase("C:\\Tools\\[x]_1.exe", "[X]_1")) << "punctuation and digits as they are";
    EXPECT_FALSE(containsIgnoreCase("rust-analyzer", "rust-analyzerX"));
    EXPECT_FALSE(containsIgnoreCase("rust", "rusty"));
    EXPECT_FALSE(containsIgnoreCase("", "a"));
    EXPECT_FALSE(containsIgnoreCase("abc", "abcd")) << "a term longer than the text";
}

TEST(AsciiCaseTest, AnEmptyTermIsInEveryText)
{
    EXPECT_TRUE(containsIgnoreCase("", ""));
    EXPECT_TRUE(containsIgnoreCase("bash", ""));
}

TEST(AsciiCaseTest, CompareOrdersLikeStringAfterFoldingAsciiLetters)
{
    EXPECT_EQ(compareIgnoreCase("", ""), std::strong_ordering::equal);
    EXPECT_EQ(compareIgnoreCase("Svchost.EXE", "svchost.exe"), std::strong_ordering::equal);
    EXPECT_EQ(compareIgnoreCase("apple", "Banana"), std::strong_ordering::less) << "case does not split the alphabet";
    EXPECT_EQ(compareIgnoreCase("Banana", "apple"), std::strong_ordering::greater);
    EXPECT_EQ(compareIgnoreCase("a", "B"), std::strong_ordering::less);
    EXPECT_EQ(compareIgnoreCase("B", "c"), std::strong_ordering::less);
    EXPECT_EQ(compareIgnoreCase("Zebra", "zebra"), std::strong_ordering::equal);
    EXPECT_EQ(compareIgnoreCase("[x]", "a"), std::strong_ordering::less) << "'[' orders before the letters, as std::string orders it";
    EXPECT_EQ(compareIgnoreCase("1", "a"), std::strong_ordering::less);
}

TEST(AsciiCaseTest, CompareOrdersAPrefixBeforeTheLongerText)
{
    EXPECT_EQ(compareIgnoreCase("", "a"), std::strong_ordering::less);
    EXPECT_EQ(compareIgnoreCase("a", ""), std::strong_ordering::greater);
    EXPECT_EQ(compareIgnoreCase("svc", "svchost"), std::strong_ordering::less);
    EXPECT_EQ(compareIgnoreCase("SVC", "svchost"), std::strong_ordering::less);
    EXPECT_EQ(compareIgnoreCase("svchost", "SVC"), std::strong_ordering::greater);
    EXPECT_EQ(compareIgnoreCase("svchost", "svchost.exe"), std::strong_ordering::less);
    EXPECT_EQ(compareIgnoreCase("svchost.exe", "SVCHOST"), std::strong_ordering::greater)
        << "a longer text with the prefix orders after it";
    EXPECT_EQ(compareIgnoreCase("svchosa", "SVCHOST"), std::strong_ordering::less) << "a differing byte decides before the length does";
}

TEST(AsciiCaseTest, CompareTreatsBytesOfEightyAndUpAsUnsigned)
{
    // std::string orders bytes as unsigned char, so "é" (C3 A9) sorts after every ASCII letter; a
    // signed-char comparison would have put it first.
    EXPECT_EQ(compareIgnoreCase("\xC3\xA9"sv, "z"), std::strong_ordering::greater);
    EXPECT_EQ(compareIgnoreCase("\xC3\xA9"sv, "Z"), std::strong_ordering::greater);
    EXPECT_EQ(compareIgnoreCase("\xC3\x89"sv, "\xC3\xA9"sv), std::strong_ordering::less) << "É and é stay distinct";
    EXPECT_EQ(compareIgnoreCase("caf\xC3\xA9"sv, "CAF\xC3\xA9"sv), std::strong_ordering::equal);
}

TEST(AsciiCaseTest, CompareAgreesWithEquals)
{
    constexpr std::string_view LEFT = "Display Name (x86)";
    for (const std::string_view right : {"display name (X86)"sv, "Display Name (x86) "sv, "Display Nam"sv, "display-name (x86)"sv})
    {
        EXPECT_EQ(compareIgnoreCase(LEFT, right) == 0, equalsIgnoreCase(LEFT, right)) << right;
    }
}

} // namespace
