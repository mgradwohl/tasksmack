#include "Core/ErrorCodeText.h"
#include "NonAsciiTestNames.h"
#include "Platform/Windows/WinString.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <string>
#include <string_view>
#include <system_error>

#include <windows.h>

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
    // A C string stops at its first NUL, as a C string does.
    EXPECT_EQ(wideToUtf8(L"Before\0After"), "Before");
}

TEST(WinStringTest, WideToUtf8WstringKeepsEmbeddedNuls)
{
    // All size() units (#4307): the std::wstring overload no longer stops at the first NUL.
    const std::string converted = wideToUtf8(std::wstring(L"a\0b", 3));
    EXPECT_EQ(converted, std::string("a\0b", 3));
    EXPECT_EQ(converted.size(), 3U);
    EXPECT_EQ(wideToUtf8(std::wstring(L"\0lead", 5)), std::string("\0lead", 5)) << "a leading NUL isn't an empty name";
    // And back: the round trip keeps every unit.
    EXPECT_EQ(utf8ToWide(converted), std::wstring(L"a\0b", 3));
}

TEST(WinStringTest, WideToDisplayUtf8ShowsEachNul)
{
    // "OneDrive\0x" shows as OneDrive<U+2400>x, never as a second "OneDrive" (#4307).
    EXPECT_EQ(wideToDisplayUtf8(std::wstring_view(L"OneDrive\0x", 10)),
              "OneDrive\xE2\x90\x80"
              "x");
    EXPECT_EQ(wideToDisplayUtf8(std::wstring_view(L"\0", 1)), "\xE2\x90\x80");
    EXPECT_EQ(wideToDisplayUtf8(std::wstring_view(L"end\0", 4)), "end\xE2\x90\x80");
    EXPECT_EQ(wideToDisplayUtf8(L"plain"), "plain");
    EXPECT_EQ(wideToDisplayUtf8({}), "");
    // A lone surrogate is U+FFFD, as wideToUtf8 shows it.
    const std::wstring lone = std::wstring(L"a") + static_cast<wchar_t>(0xD800);
    EXPECT_EQ(wideToDisplayUtf8(lone), "a\xEF\xBF\xBD");
}

TEST(WinStringTest, HasLoneSurrogateFindsOnlyUnpairedHalves)
{
    // #4310: what can't survive the trip to UTF-8 and back.
    static_assert(!hasLoneSurrogate(L""));
    static_assert(!hasLoneSurrogate(L"TaskSmack.exe"));
    EXPECT_FALSE(hasLoneSurrogate(L"Größe 日本語 😀")) << "a pair is a character";
    const auto unit = [](unsigned value)
    {
        return static_cast<wchar_t>(value);
    };
    EXPECT_TRUE(hasLoneSurrogate(std::wstring{L'a', unit(0xD800)})) << "a high half at the end";
    EXPECT_TRUE(hasLoneSurrogate(std::wstring{unit(0xD800), L'a'})) << "a high half before a non-surrogate";
    EXPECT_TRUE(hasLoneSurrogate(std::wstring{unit(0xDC00), L'a'})) << "a low half first";
    EXPECT_TRUE(hasLoneSurrogate(std::wstring{unit(0xD83D), unit(0xDE00), unit(0xDC00)})) << "a pair, then a low half";
    EXPECT_TRUE(hasLoneSurrogate(std::wstring{unit(0xD800), unit(0xD800), unit(0xDC00)})) << "a high half, then a pair";
    EXPECT_EQ(wideToUtf8(std::wstring{L'a', unit(0xD800)}), "a\xEF\xBF\xBD") << "why: it comes back as U+FFFD";
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

// ========== Non-ASCII round trips (#1648, slice C) ==========

TEST(WinStringTest, NonBmpCharacterIsASurrogatePair)
{
    // U+1F525 FIRE: four UTF-8 bytes, two UTF-16 units (a surrogate pair), and back.
    const std::wstring wide = utf8ToWide(::TestSupport::EMOJI_NAME);
    ASSERT_EQ(wide.size(), 2U);
    EXPECT_EQ(wide[0], static_cast<wchar_t>(0xD83D));
    EXPECT_EQ(wide[1], static_cast<wchar_t>(0xDD25));
    EXPECT_EQ(wideToUtf8(wide), ::TestSupport::EMOJI_NAME);
    EXPECT_EQ(wideToUtf8(std::wstring_view{wide}), ::TestSupport::EMOJI_NAME);
}

TEST(WinStringTest, EveryTestScriptRoundTrips)
{
    for (const std::string_view name : ::TestSupport::NON_ASCII_NAMES)
    {
        EXPECT_EQ(wideToUtf8(utf8ToWide(name)), name);
    }
    const std::string all = ::TestSupport::allNonAsciiNames();
    EXPECT_EQ(utf8ToWide(all), L"任务管理器🔥Ünïcödéمدير");
    EXPECT_EQ(wideToUtf8(utf8ToWide(all)), all);
}

TEST(WinStringTest, UnpairedSurrogateBecomesReplacementCharacter)
{
    // NTFS and process names can hold an unpaired surrogate. It has no UTF-8 form, so it comes out
    // as U+FFFD (EF BF BD) rather than failing the whole string or crashing.
    const std::wstring loneHigh{L'a', static_cast<wchar_t>(0xD83D), L'b'};
    const std::wstring loneLow{L'a', static_cast<wchar_t>(0xDD25), L'b'};
    const std::string expected = "a\xEF\xBF\xBD"
                                 "b";
    EXPECT_EQ(wideToUtf8(loneHigh), expected);
    EXPECT_EQ(wideToUtf8(std::wstring_view{loneLow}), expected);
}

TEST(WinStringTest, Utf8ToWideRejectsEveryKindOfInvalidUtf8)
{
    // Strict on purpose: the result names a file, service or registry value for a W API, and a
    // guessed one could name a different object. Empty, never a crash or a partial string.
    const std::string_view overlongSlash = "\xC0\xAF";
    const std::string_view encodedSurrogate = "\xED\xA0\x80";
    const std::string_view aboveMaxCodePoint = "\xF4\x90\x80\x80";
    const std::string_view cutEmoji = "\xF0\x9F\x94";
    const std::string_view codePage1252 = "\xDC"
                                          "n\xEF"
                                          "c\xF6"
                                          "d\xE9"; // "Ünïcödé" as code page 1252 bytes
    const std::string_view trailingFF = "ok\xFF";
    for (const std::string_view invalid : {overlongSlash, encodedSurrogate, aboveMaxCodePoint, cutEmoji, codePage1252, trailingFF})
    {
        EXPECT_EQ(utf8ToWide(invalid), L"");
    }
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
    const std::wstring longWide(1000, L'A');
    const std::string longUtf8(1000, 'A');
    EXPECT_EQ(wideToUtf8(longWide), longUtf8);
}

TEST(WinStringTest, Utf8ToWideLongStringConvertsCorrectly)
{
    // Test a longer string
    const std::string longUtf8(1000, 'B');
    const std::wstring longWide(1000, L'B');
    EXPECT_EQ(utf8ToWide(longUtf8), longWide);
}

// ========== win32ErrorText Tests (#4308) ==========

// TaskSmackTests has no activeCodePage manifest, so on a cp1252 or cp932 system std::system_category().message() would
// hand back code-page bytes here; win32ErrorText() must give UTF-8 whatever the code page.
TEST(WinStringTest, Win32ErrorTextIsValidUtf8WithNoTrailingBreakOrPeriod)
{
    for (const DWORD code : {
             DWORD{ERROR_ACCESS_DENIED},
             DWORD{ERROR_FILE_NOT_FOUND},
             DWORD{ERROR_INVALID_PARAMETER},
             DWORD{ERROR_SHARING_VIOLATION},
             DWORD{ERROR_NOT_ENOUGH_MEMORY},
         })
    {
        const std::string text = win32ErrorText(code);
        ASSERT_FALSE(text.empty()) << code;
        // utf8ToWide() rejects invalid UTF-8 (MB_ERR_INVALID_CHARS) with an empty result.
        EXPECT_FALSE(utf8ToWide(text).empty()) << code << ": " << text;
        EXPECT_NE(text.back(), '\n') << code;
        EXPECT_NE(text.back(), '\r') << code;
        EXPECT_NE(text.back(), '.') << code;
        EXPECT_NE(text.back(), ' ') << code;
        EXPECT_FALSE(text.starts_with("error ")) << code << " has system text: " << text;
    }
}

TEST(WinStringTest, Win32ErrorTextMatchesFormatMessageW)
{
    std::array<wchar_t, 512> wide{};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr,
                                        ERROR_ACCESS_DENIED,
                                        0,
                                        wide.data(),
                                        static_cast<DWORD>(wide.size()),
                                        nullptr);
    ASSERT_GT(length, 0U);
    const std::string expected = wideToUtf8(std::wstring_view(wide.data(), length));
    EXPECT_TRUE(expected.starts_with(win32ErrorText(ERROR_ACCESS_DENIED))) << expected;
}

TEST(WinStringTest, Win32ErrorTextFallsBackToTheNumber)
{
    // A code in the customer range (bit 29) that no system message table has.
    EXPECT_EQ(win32ErrorText(0x2000'FFFFUL), "error 536936447");
}

TEST(WinStringTest, ErrorCodeTextSendsSystemCategoryThroughFormatMessageW)
{
    const std::error_code denied(ERROR_ACCESS_DENIED, std::system_category());
    EXPECT_EQ(Core::errorCodeText(denied), win32ErrorText(ERROR_ACCESS_DENIED));
    // A generic (errno) code keeps message(): the CRT's table, not FormatMessage.
    const std::error_code noEntry(ENOENT, std::generic_category());
    EXPECT_EQ(Core::errorCodeText(noEntry), noEntry.message());
}

} // namespace
} // namespace Platform::WinString
