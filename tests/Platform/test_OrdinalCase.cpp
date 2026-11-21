/// @file test_OrdinalCase.cpp
/// @brief equalsOrdinalIgnoreCase (#4309): how Windows compares file paths, process names and account names,
/// ordinal and case-insensitive over UTF-16, so "É" equals "é" where an ASCII fold says they differ. Text that
/// isn't valid UTF-8 is compared exactly. And samePathIgnoringTrailingSeparators() (#4699), the elevated code's one path
/// comparison built on it.

#include "Platform/Windows/OrdinalCase.h"
#include "Platform/Windows/WinString.h"

#include <gtest/gtest.h>

#include <format>
#include <string>
#include <string_view>
#include <vector>

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

/// Two paths, and whether samePathIgnoringTrailingSeparators() names them the same.
struct PathPair
{
    std::wstring_view first;
    std::wstring_view second;
    bool same = false;
};

// The elevated code's one path comparison (#4699): trailing separators, either kind and any number, don't count, except
// on a root, where one does; case folds as equalsOrdinalIgnoreCase() folds it; nothing else is normalized.
TEST(OrdinalCaseTest, PathsCompareIgnoringTrailingSeparatorsExceptARoots)
{
    const std::vector<PathPair> rows{
        // Trailing separators, of either kind, and several.
        {.first = L"C:\\a\\", .second = L"C:\\a", .same = true},
        {.first = L"C:\\a/", .second = L"C:\\a", .same = true},
        {.first = L"C:\\a\\\\/", .second = L"C:\\a", .same = true},
        {.first = L"C:\\a\\", .second = L"C:\\a/", .same = true},
        {.first = L"C:\\a", .second = L"C:\\a", .same = true},
        {.first = L"C:\\a\\b", .second = L"C:\\a", .same = false},
        {.first = L"C:\\ab", .second = L"C:\\a\\", .same = false},
        // A drive root keeps its separator: "C:\" is the root folder, "C:" the drive (its current folder).
        {.first = L"C:\\", .second = L"C:\\", .same = true},
        {.first = L"C:\\", .second = L"C:/", .same = true},
        {.first = L"C:\\\\", .second = L"C:\\", .same = true},
        {.first = L"c:\\", .second = L"C:\\", .same = true},
        {.first = L"C:\\", .second = L"C:", .same = false},
        {.first = L"C:", .second = L"C:", .same = true},
        {.first = L"C:\\", .second = L"D:\\", .same = false},
        // UNC: a share's root folder is the share.
        {.first = L"\\\\server\\share\\", .second = L"\\\\server\\share", .same = true},
        {.first = L"\\\\SERVER\\Share\\dir\\", .second = L"\\\\server\\share\\DIR", .same = true},
        {.first = L"\\\\server\\share", .second = L"\\\\server\\share2", .same = false},
        // \\?\ and \\.\ device paths: a drive's root keeps its separator there too, and the prefix is compared as written.
        {.first = L"\\\\?\\C:\\a\\", .second = L"\\\\?\\c:\\A", .same = true},
        {.first = L"\\\\?\\C:\\", .second = L"\\\\?\\C:\\", .same = true},
        {.first = L"\\\\?\\C:\\", .second = L"\\\\?\\C:", .same = false},
        {.first = L"\\\\.\\C:\\", .second = L"\\\\.\\C:", .same = false},
        {.first = L"\\\\?\\UNC\\server\\share\\", .second = L"\\\\?\\UNC\\server\\share", .same = true},
        {.first = L"\\\\?\\C:\\a", .second = L"C:\\a", .same = false},
        // Case folds as NTFS folds it, and only case.
        {.first = L"C:\\Users\\\u00C9mile\\", .second = L"c:\\users\\\u00E9mile", .same = true},
        {.first = L"C:\\\u00C9", .second = L"C:\\E", .same = false},
        // Empty, and separators alone (the current drive's root): one separator counts.
        {.first = L"", .second = L"", .same = true},
        {.first = L"\\", .second = L"/", .same = true},
        {.first = L"\\", .second = L"\\\\", .same = true},
        {.first = L"\\", .second = L"", .same = false},
        {.first = L"C:\\a", .second = L"", .same = false},
        // An unpaired surrogate is compared as itself, a trailing separator after it dropped.
        {.first = L"C:\\a\xD800\\", .second = L"C:\\A\xD800", .same = true},
        {.first = L"C:\\a\xD800", .second = L"C:\\a\xDC00", .same = false},
    };
    for (const PathPair& row : rows)
    {
        const std::string shown = std::format("{} vs {}", WinString::wideToUtf8(row.first), WinString::wideToUtf8(row.second));
        EXPECT_EQ(samePathIgnoringTrailingSeparators(row.first, row.second), row.same) << shown;
        EXPECT_EQ(samePathIgnoringTrailingSeparators(row.second, row.first), row.same) << shown << " (swapped)";
    }
    // Only each view's own length is read, never up to a NUL or past a separator it ends before.
    constexpr std::wstring_view LONGER = L"C:\\Dumps\\more";
    EXPECT_TRUE(samePathIgnoringTrailingSeparators(LONGER.substr(0, 9), L"c:\\dumps"));
    EXPECT_FALSE(samePathIgnoringTrailingSeparators(LONGER, L"c:\\dumps"));
}

} // namespace
} // namespace Platform::Windows
