/// @file test_OrdinalCase.cpp
/// @brief equalsOrdinalIgnoreCase (#4309): how Windows compares file paths, process names and account names,
/// ordinal and case-insensitive over UTF-16, so "É" equals "é" where an ASCII fold says they differ. Text that
/// isn't valid UTF-8 is compared exactly.

#include "Platform/Windows/OrdinalCase.h"

#include <gtest/gtest.h>

#include <string_view>

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

// The UTF-16 form the elevated code's paths and account names use (#4368): the same fold, on the text as it is, the
// whole of it (a trailing separator counts), an unpaired surrogate compared as itself.
TEST(OrdinalCaseTest, Utf16TextFoldsAsWindowsFoldsIt)
{
    using std::wstring_view;
    EXPECT_TRUE(equalsOrdinalIgnoreCase(wstring_view(L"C:\\Users\\\u00C9mile"), wstring_view(L"c:\\users\\\u00E9mile")));
    EXPECT_TRUE(equalsOrdinalIgnoreCase(wstring_view(L"TaskSmack.exe"), wstring_view(L"TASKSMACK.EXE")));
    EXPECT_TRUE(equalsOrdinalIgnoreCase(wstring_view(L""), wstring_view(L"")));
    EXPECT_FALSE(equalsOrdinalIgnoreCase(wstring_view(L"C:\\Dumps"), wstring_view(L"C:\\Dumps\\"))) << "a trailing separator counts";
    EXPECT_FALSE(equalsOrdinalIgnoreCase(wstring_view(L"C:\\Dumps"), wstring_view(L"D:\\Dumps")));
    EXPECT_FALSE(equalsOrdinalIgnoreCase(wstring_view(L"\u00C9"), wstring_view(L"E"))) << "no accent stripping: it's ordinal";
    EXPECT_TRUE(equalsOrdinalIgnoreCase(wstring_view(L"a\xD800"), wstring_view(L"A\xD800")));
    EXPECT_FALSE(equalsOrdinalIgnoreCase(wstring_view(L"a\xD800"), wstring_view(L"a\xDC00"))) << "unpaired surrogates aren't one";
    // Only each view's own length is read, never up to a NUL.
    constexpr wstring_view LONGER = L"C:\\Dumps\\more";
    EXPECT_TRUE(equalsOrdinalIgnoreCase(LONGER.substr(0, 8), wstring_view(L"c:\\dumps")));
}

} // namespace
} // namespace Platform::Windows
