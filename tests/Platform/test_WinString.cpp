#include "Platform/Windows/WinString.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace Platform::WinString
{
namespace
{

// ========== wideToUtf8 Tests ==========

TEST(WinStringTest, WideToUtf8NullReturnsEmpty)
{
    EXPECT_EQ(wideToUtf8(nullptr), "");
}

TEST(WinStringTest, WideToUtf8EmptyReturnsEmpty)
{
    EXPECT_EQ(wideToUtf8(L""), "");
    EXPECT_EQ(wideToUtf8(std::wstring{}), "");
}

TEST(WinStringTest, WideToUtf8AsciiConvertsCorrectly)
{
    EXPECT_EQ(wideToUtf8(L"Hello"), "Hello");
    EXPECT_EQ(wideToUtf8(L"TaskSmack"), "TaskSmack");
    EXPECT_EQ(wideToUtf8(L"C:\\Windows\\System32"), "C:\\Windows\\System32");
}

TEST(WinStringTest, WideToUtf8BasicUnicodeConvertsCorrectly)
{
    // German umlauts (BMP characters)
    EXPECT_EQ(wideToUtf8(L"Größe"), "Größe");
    // French accents
    EXPECT_EQ(wideToUtf8(L"café"), "café");
    // Spanish
    EXPECT_EQ(wideToUtf8(L"año"), "año");
}

TEST(WinStringTest, WideToUtf8CjkConvertsCorrectly)
{
    // Chinese characters
    EXPECT_EQ(wideToUtf8(L"中文"), "中文");
    // Japanese
    EXPECT_EQ(wideToUtf8(L"日本語"), "日本語");
    // Korean
    EXPECT_EQ(wideToUtf8(L"한글"), "한글");
}

TEST(WinStringTest, WideToUtf8EmojiConvertsCorrectly)
{
    // Emoji (surrogate pairs in UTF-16)
    EXPECT_EQ(wideToUtf8(L"😀"), "😀");
    EXPECT_EQ(wideToUtf8(L"🎉"), "🎉");
}

TEST(WinStringTest, WideToUtf8MixedContentConvertsCorrectly)
{
    EXPECT_EQ(wideToUtf8(L"Hello 世界!"), "Hello 世界!");
    EXPECT_EQ(wideToUtf8(L"Test: café & 日本語"), "Test: café & 日本語");
}

TEST(WinStringTest, WideToUtf8SpecialCharactersConvertsCorrectly)
{
    // Newlines and tabs
    EXPECT_EQ(wideToUtf8(L"Line1\nLine2\tTab"), "Line1\nLine2\tTab");
    // Null embedded (should stop at first null)
    EXPECT_EQ(wideToUtf8(L"Before"), "Before");
}

// ========== wideToUtf8 (wstring_view) Tests ==========

TEST(WinStringTest, WideToUtf8ViewEmptyReturnsEmpty)
{
    EXPECT_EQ(wideToUtf8(std::wstring_view{}), "");
    EXPECT_EQ(wideToUtf8(std::wstring_view(L"")), "");
}

TEST(WinStringTest, WideToUtf8ViewConvertsCorrectly)
{
    EXPECT_EQ(wideToUtf8(std::wstring_view(L"TaskSmack.exe")), "TaskSmack.exe");
    EXPECT_EQ(wideToUtf8(std::wstring_view(L"Größe 日本語 😀")), "Größe 日本語 😀");
}

TEST(WinStringTest, WideToUtf8ViewHandlesNonNullTerminatedSubstring)
{
    // Views over counted OS strings (e.g. UNICODE_STRING) are not null-terminated;
    // conversion must honor the view length, not scan for a terminator.
    const std::wstring backing = L"chrome.exe#garbage";
    const std::wstring_view view(backing.data(), 10);
    EXPECT_EQ(wideToUtf8(view), "chrome.exe");
}

// ========== utf8ToWide Tests ==========

TEST(WinStringTest, Utf8ToWideEmptyReturnsEmpty)
{
    EXPECT_EQ(utf8ToWide(""), L"");
    EXPECT_EQ(utf8ToWide(std::string_view{}), L"");
}

TEST(WinStringTest, Utf8ToWideAsciiConvertsCorrectly)
{
    EXPECT_EQ(utf8ToWide("Hello"), L"Hello");
    EXPECT_EQ(utf8ToWide("TaskSmack"), L"TaskSmack");
    EXPECT_EQ(utf8ToWide("C:\\Windows\\System32"), L"C:\\Windows\\System32");
}

TEST(WinStringTest, Utf8ToWideBasicUnicodeConvertsCorrectly)
{
    // German umlauts
    EXPECT_EQ(utf8ToWide("Größe"), L"Größe");
    // French accents
    EXPECT_EQ(utf8ToWide("café"), L"café");
}

TEST(WinStringTest, Utf8ToWideCjkConvertsCorrectly)
{
    // Chinese characters
    EXPECT_EQ(utf8ToWide("中文"), L"中文");
    // Japanese
    EXPECT_EQ(utf8ToWide("日本語"), L"日本語");
}

TEST(WinStringTest, Utf8ToWideEmojiConvertsCorrectly)
{
    // Emoji (become surrogate pairs in UTF-16)
    EXPECT_EQ(utf8ToWide("😀"), L"😀");
}

// ========== Round-trip Tests ==========

TEST(WinStringTest, RoundTripPreservesAscii)
{
    const std::string original = "Hello, World!";
    EXPECT_EQ(wideToUtf8(utf8ToWide(original)), original);
}

TEST(WinStringTest, RoundTripPreservesUnicode)
{
    const std::string original = "Größe: 日本語 café 😀";
    EXPECT_EQ(wideToUtf8(utf8ToWide(original)), original);
}

TEST(WinStringTest, RoundTripPreservesWindowsPaths)
{
    const std::string original = "C:\\Users\\Günther\\Documents\\日本語フォルダ\\file.txt";
    EXPECT_EQ(wideToUtf8(utf8ToWide(original)), original);
}

// ========== Edge Cases ==========

TEST(WinStringTest, Utf8ToWideInvalidUtf8ReturnsEmpty)
{
    // Invalid UTF-8 sequences should return empty string
    // 0x80 alone is invalid UTF-8 (continuation byte without start)
    const std::string_view invalid("\x80\x81\x82", 3);
    EXPECT_EQ(utf8ToWide(invalid), L"");
}

TEST(WinStringTest, Utf8ToWideTruncatedSequenceReturnsEmpty)
{
    // Truncated 2-byte UTF-8 sequence (ö starts with 0xC3 but needs second byte)
    const std::string_view truncated("\xC3", 1);
    EXPECT_EQ(utf8ToWide(truncated), L"");
}

TEST(WinStringTest, WideToUtf8LongStringConvertsCorrectly)
{
    // Test a longer string to verify buffer handling
    std::wstring longWide(1000, L'A');
    std::string longUtf8(1000, 'A');
    EXPECT_EQ(wideToUtf8(longWide), longUtf8);
}

TEST(WinStringTest, Utf8ToWideLongStringConvertsCorrectly)
{
    // Test a longer string
    std::string longUtf8(1000, 'B');
    std::wstring longWide(1000, L'B');
    EXPECT_EQ(utf8ToWide(longUtf8), longWide);
}

} // namespace
} // namespace Platform::WinString
