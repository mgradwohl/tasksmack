/// @file test_OrdinalCase.cpp
/// @brief equalsOrdinalIgnoreCase (#4309): how Windows compares file paths, process names and account names,
/// ordinal and case-insensitive over UTF-16, so "É" equals "é" where an ASCII fold says they differ. Text that
/// isn't valid UTF-8 is compared exactly.

#include "Platform/Windows/OrdinalCase.h"

#include <gtest/gtest.h>

namespace Platform::Windows
{
namespace
{

TEST(OrdinalCaseTest, NonAsciiLettersFoldAsWindowsFoldsThem)
{
    EXPECT_TRUE(equalsOrdinalIgnoreCase("\xC3\x89", "\xC3\xA9")); // É, é
    EXPECT_TRUE(equalsOrdinalIgnoreCase("C:\\Users\\\xC3\x89mile", "c:\\users\\\xC3\xA9mile"));
    EXPECT_TRUE(equalsOrdinalIgnoreCase("\xC3\x9C"
                                        "BER.EXE",
                                        "\xC3\x9C"
                                        "ber.exe"));
    EXPECT_TRUE(equalsOrdinalIgnoreCase("\xC3\x9C"
                                        "BER.EXE",
                                        "\xC3\xBC"
                                        "ber.exe"))
        << "Ü and ü";
}

TEST(OrdinalCaseTest, AsciiIgnoresCase)
{
    EXPECT_TRUE(equalsOrdinalIgnoreCase("TaskSmack.exe", "TASKSMACK.EXE"));
    EXPECT_TRUE(equalsOrdinalIgnoreCase("same", "same"));
}

TEST(OrdinalCaseTest, DifferentTextIsNotEqual)
{
    EXPECT_FALSE(equalsOrdinalIgnoreCase("a.exe", "b.exe"));
    EXPECT_FALSE(equalsOrdinalIgnoreCase("a.exe", "a.exe2"));
    EXPECT_FALSE(equalsOrdinalIgnoreCase("\xC3\x89", "E")) << "no accent stripping: it's ordinal";
    EXPECT_FALSE(equalsOrdinalIgnoreCase("a", ""));
}

TEST(OrdinalCaseTest, EmptyTextsAreEqual)
{
    EXPECT_TRUE(equalsOrdinalIgnoreCase("", ""));
}

TEST(OrdinalCaseTest, InvalidUtf8IsComparedExactly)
{
    EXPECT_TRUE(equalsOrdinalIgnoreCase("a\xFF", "a\xFF"));
    EXPECT_FALSE(equalsOrdinalIgnoreCase("A\xFF", "a\xFF")) << "no folding of text that isn't UTF-8";
    EXPECT_FALSE(equalsOrdinalIgnoreCase("a\xFF", "a\xFE"));
}

} // namespace
} // namespace Platform::Windows
