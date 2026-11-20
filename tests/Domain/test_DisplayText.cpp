// Display text for raw OS bytes (#4328): U+FFFD for malformed UTF-8, escapes for controls and the backslash, and
// nmcli's rule for Wi-Fi network names.

#include "Domain/DisplayText.h"
#include "Domain/Utf8Sanitize.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

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
    EXPECT_EQ(DisplayText::escaped("C:\\Windows"), "C:\\\\Windows");
    EXPECT_EQ(DisplayText::escaped("\t\r\x7F"), "\\t\\r\\x7F");
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
