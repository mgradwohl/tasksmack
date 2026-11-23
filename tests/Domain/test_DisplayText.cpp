// Display text for raw OS bytes (#4328): U+FFFD for malformed UTF-8, escapes for controls and for a backslash that
// would read as one (#4838), and nmcli's rule for Wi-Fi network names.

#include "Domain/DisplayText.h"
#include "Domain/Utf8Sanitize.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

namespace
{

namespace DisplayText = Domain::DisplayText;

constexpr std::string_view FFFD = "\xEF\xBF\xBD";

TEST(DisplayTextTest, MalformedUtf8BecomesValidUtf8WithAReplacement)
{
    EXPECT_EQ(DisplayText::escaped("Caf\xE9.txt"), std::string("Caf") + std::string(FFFD) + ".txt");
    EXPECT_EQ(DisplayText::escaped("\xE2\x82"), std::string(FFFD)) << "a truncated sequence is one replacement";
    EXPECT_TRUE(Domain::isValidUtf8(DisplayText::escaped("a\xFF\xC0\xAF\xED\xA0\x80z")));
    EXPECT_EQ(DisplayText::escaped("caf\xC3\xA9 \xF0\x9F\x98\x80"), "caf\xC3\xA9 \xF0\x9F\x98\x80") << "valid text is kept";
}

TEST(DisplayTextTest, ALiteralBackslashAndALiteralEscapeStayUnambiguous)
{
    // A real newline, and the two characters '\' 'n', show differently.
    EXPECT_EQ(DisplayText::escaped("a\nb"), "a\\nb");
    EXPECT_EQ(DisplayText::escaped("a\\nb"), "a\\\\nb");
    EXPECT_NE(DisplayText::escaped("a\nb"), DisplayText::escaped("a\\nb"));
    // So do a control byte and the text "\x01".
    EXPECT_EQ(DisplayText::escaped("\x01"), "\\x01");
    EXPECT_EQ(DisplayText::escaped("\\x01"), "\\\\x01");
    EXPECT_EQ(DisplayText::escaped("\t\r\x7F"), "\\t\\r\\x7F");
    // And '\' before 't' or 'r', and a backslash just before a control byte, whose escape starts with '\'.
    EXPECT_EQ(DisplayText::escaped("\\t\\r"), "\\\\t\\\\r");
    EXPECT_EQ(DisplayText::escaped("a\\\n"), "a\\\\\\n") << "a backslash, then a real newline";
}

// #4838: only a backslash that would read as an escape is doubled, so a Windows path shows as itself.
TEST(DisplayTextTest, APathsBackslashesShowAsThemselves)
{
    EXPECT_EQ(DisplayText::escaped("C:\\Windows\\system32\\cmd.exe"), "C:\\Windows\\system32\\cmd.exe");
    EXPECT_EQ(DisplayText::escaped("CORP\\matt"), "CORP\\matt");
    EXPECT_EQ(DisplayText::escaped("C:\\Users\\"), "C:\\Users\\") << "a trailing backslash shows unchanged";
    EXPECT_EQ(DisplayText::escaped("\\"), "\\");
    // A backslash before 'n' still can't pass for a newline, even in a path.
    EXPECT_EQ(DisplayText::escaped("C:\\new"), "C:\\\\new");
}

// Two backslashes can't read as one escaped backslash ("\\"): the first is doubled, the second stands alone.
TEST(DisplayTextTest, TwoBackslashesNeverReadAsOneEscapedBackslash)
{
    EXPECT_EQ(DisplayText::escaped("\\\\"), "\\\\\\");
    EXPECT_NE(DisplayText::escaped("\\\\"), "\\\\");
    EXPECT_EQ(DisplayText::escaped("\\\\server\\share"), "\\\\\\server\\share");
    EXPECT_EQ(DisplayText::escaped("\\\\n"), "\\\\\\\\n") << "two backslashes, then 'n'";
}

/// The decoder the file comment describes: "\\" is one backslash; "\n", "\t", "\r" and "\xNN" are control characters;
/// a '\' before anything else, or at the end, is a backslash on its own.
[[nodiscard]] std::string decoded(std::string_view shown)
{
    std::string out;
    std::size_t at = 0;
    while (at < shown.size())
    {
        if (shown[at] != '\\' || at + 1 == shown.size())
        {
            out += shown[at++];
            continue;
        }
        switch (shown[at + 1])
        {
        case '\\':
            out += '\\';
            at += 2;
            break;
        case 'n':
            out += '\n';
            at += 2;
            break;
        case 't':
            out += '\t';
            at += 2;
            break;
        case 'r':
            out += '\r';
            at += 2;
            break;
        case 'x':
        {
            unsigned int byte = 0;
            const std::string_view digits = shown.substr(at + 2, 2);
            const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), byte, 16);
            if (error != std::errc{} || end != digits.data() + 2)
            {
                return "<not an escape>";
            }
            out += static_cast<char>(byte);
            at += 4;
            break;
        }
        default:
            out += '\\';
            ++at;
            break;
        }
    }
    return out;
}

TEST(DisplayTextTest, TheDecoderReadsBackTheExamples)
{
    for (const std::string_view raw : {"C:\\Windows\\system32", "\\\\", "\\", "a\\nb", "\\x41", "\\\n", R"(x\\\y\)"})
    {
        EXPECT_EQ(decoded(DisplayText::escaped(raw)), raw) << DisplayText::escaped(raw);
    }
}

/// Text built from the characters that matter to the rule: backslashes, the escape letters, hex digits, control bytes,
/// other ASCII and multi-byte characters. Valid UTF-8 with no U+FFFD by construction.
[[nodiscard]] rc::Gen<std::string> textNearEscapes()
{
    const std::vector<std::string> pieces{
        "\\",
        "n",
        "t",
        "r",
        "x",
        "4",
        "1",
        "F",
        "\n",
        "\t",
        "\r",
        "\x01",
        "\x1B",
        "\x7F",
        std::string(1, '\0'),
        "C:",
        " ",
        "caf\xC3\xA9",
        "\xE2\x98\x95",
        "\xF0\x9F\x98\x80",
    };
    return rc::gen::map(rc::gen::container<std::vector<std::string>>(rc::gen::oneOf(
                            rc::gen::elementOf(pieces),
                            rc::gen::map(rc::gen::inRange<int>(0, 0x80), [](int c) { return std::string(1, static_cast<char>(c)); }))),
                        [](const std::vector<std::string>& parts)
                        {
                            std::string text;
                            for (const std::string& part : parts)
                            {
                                text += part;
                            }
                            return text;
                        });
}

// For any valid UTF-8 without U+FFFD, the documented rule decodes the display text back to the raw text, and the
// display text is one line of valid UTF-8 with no control bytes left in it.
RC_GTEST_PROP(DisplayTextPropertiesTest, TheDocumentedRuleRoundTripsTheDisplayText, ())
{
    const std::string raw = *textNearEscapes();
    RC_PRE(Domain::isValidUtf8(raw));
    const std::string shown = DisplayText::escaped(raw);
    RC_ASSERT(Domain::isValidUtf8(shown));
    RC_ASSERT(std::ranges::none_of(shown, [](char c) { return static_cast<unsigned char>(c) < 0x20U || c == '\x7F'; }));
    RC_ASSERT(decoded(shown) == raw);
}

// Any bytes at all: still valid UTF-8 on one line.
RC_GTEST_PROP(DisplayTextPropertiesTest, AnyBytesShowAsOneLineOfValidUtf8, (const std::string& raw))
{
    const std::string shown = DisplayText::escaped(raw);
    RC_ASSERT(Domain::isValidUtf8(shown));
    RC_ASSERT(std::ranges::none_of(shown, [](char c) { return static_cast<unsigned char>(c) < 0x20U || c == '\x7F'; }));
}

TEST(DisplayTextTest, TheRawBytesRoundTripThroughTheHexCopy)
{
    const std::string raw("Caf\xE9\x00\xFF\n", 7);
    const std::string hex = DisplayText::hex(raw);
    EXPECT_EQ(hex, "436166E900FF0A");
    std::string back;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        back += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
    }
    EXPECT_EQ(back, raw);
}

TEST(DisplayTextTest, AWifiNetworkNameShowsAsNmcliShowsIt)
{
    EXPECT_EQ(DisplayText::ssid("HomeNet"), "HomeNet");
    EXPECT_EQ(DisplayText::ssid("Caf\xC3\xA9 \xE2\x98\x95"), "Caf\xC3\xA9 \xE2\x98\x95") << "valid non-ASCII is text";
    // Not valid UTF-8: the whole name in hex, never text mixed with replacements, so two names never look the same.
    EXPECT_EQ(DisplayText::ssid("Cafe \xFF"), "4361666520FF");
    EXPECT_NE(DisplayText::ssid("Net\xFF"), DisplayText::ssid("Net\xFE"));
    EXPECT_EQ(DisplayText::escaped("Net\xFF"), DisplayText::escaped("Net\xFE")) << "which escaped() alone would make the same";
}

} // namespace
