#include "UI/Clipboard.h"

#include <gtest/gtest.h>

#include <string>

namespace UI
{
namespace
{

std::string& copied()
{
    static std::string text;
    return text;
}

void captureClipboard(const char* text)
{
    copied() = text;
}

// #4325: Copy Command Line on "vim caf\xE9.txt" put ill-formed UTF-8 on the clipboard.
TEST(ClipboardTest, IllFormedTextIsCopiedWithReplacementCharacters)
{
    setClipboardUtf8("vim caf\xE9.txt", &captureClipboard);
    EXPECT_EQ(copied(), "vim caf\xEF\xBF\xBD.txt");
    setClipboardUtf8("QQ\xE9\x9F\xB3\xE5", &captureClipboard); // a comm cut inside a character
    EXPECT_EQ(copied(), "QQ\xE9\x9F\xB3\xEF\xBF\xBD");
}

TEST(ClipboardTest, WellFormedTextIsCopiedAsItIs)
{
    setClipboardUtf8("caf\xC3\xA9 \xE2\x9C\x93", &captureClipboard);
    EXPECT_EQ(copied(), "caf\xC3\xA9 \xE2\x9C\x93");
    setClipboardUtf8("", &captureClipboard);
    EXPECT_EQ(copied(), "");
}

TEST(ClipboardTest, ClearEmptiesIt)
{
    copied() = "previous";
    clearClipboard(&captureClipboard);
    EXPECT_EQ(copied(), "");
}

} // namespace
} // namespace UI
