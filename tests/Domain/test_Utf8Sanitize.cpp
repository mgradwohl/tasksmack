#include "Domain/SingleLineText.h"
#include "Domain/Utf8Sanitize.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace Domain
{
namespace
{

constexpr std::string_view FFFD = UTF8_REPLACEMENT_CHARACTER;

std::string withFffd(std::string_view before, int count, std::string_view after)
{
    std::string out(before);
    for (int i = 0; i < count; ++i)
    {
        out += FFFD;
    }
    out += after;
    return out;
}

// --- Well-formed text is untouched ---

TEST(Utf8SanitizeTest, WellFormedTextIsUnchanged)
{
    for (const std::string_view text : {std::string_view(""),
                                        std::string_view("/usr/bin/bash --login"),
                                        std::string_view("café — ünïcödé ✓"),
                                        std::string_view("QQ音乐播放器"),
                                        std::string_view("\xF0\x9F\x98\x80 emoji"),
                                        std::string_view("\xF4\x8F\xBF\xBF")})
    {
        EXPECT_TRUE(isValidUtf8(text)) << text;
        EXPECT_EQ(sanitizedUtf8(text), text);
        std::string out;
        EXPECT_FALSE(appendSanitizedUtf8(out, text));
    }
}

// --- The cases #4301 names ---

TEST(Utf8SanitizeTest, ACommCutInTheMiddleOfACjkCharacterEndsInOneReplacement)
{
    // Linux keeps 15 bytes of a comm: QQ (2) + 音乐播放 (12) + the first byte of 器 (E5).
    const std::string comm = std::string("QQ音乐播放器").substr(0, 15);
    ASSERT_EQ(comm.size(), 15U);
    EXPECT_FALSE(isValidUtf8(comm));
    EXPECT_EQ(sanitizedUtf8(comm), withFffd("QQ音乐播放", 1, ""));
}

TEST(Utf8SanitizeTest, ALoneLatin1ByteKeepsTheBytesAfterIt)
{
    // ImGui's own decoder swallowed ".t" after the E9 ("Caf<?>xt"): one replacement, then the rest as is.
    EXPECT_EQ(sanitizedUtf8("Caf\xE9.txt"), withFffd("Caf", 1, ".txt"));
    EXPECT_EQ(sanitizedUtf8("vlc Gr\xFC\xDF"
                            "e.mp3"),
              withFffd("vlc Gr", 2, "e.mp3"));
}

TEST(Utf8SanitizeTest, OverlongSurrogateAndOutOfRangeFormsAreReplaced)
{
    // Maximal subparts (Unicode 3.9, Table 3-8): C0 is never a lead, so C0 AF is two subparts.
    EXPECT_EQ(sanitizedUtf8("\xC0\xAF"), withFffd("", 2, ""));
    // ED A0 80 encodes the surrogate U+D800: ED's second byte must be 80..9F, so three replacements.
    EXPECT_EQ(sanitizedUtf8("\xED\xA0\x80"), withFffd("", 3, ""));
    EXPECT_EQ(sanitizedUtf8("\xFF"), withFffd("", 1, ""));
    EXPECT_EQ(sanitizedUtf8("a\xF5\x80"
                            "b"),
              withFffd("a", 2, "b"));
    // F4 90 80 80 would be past U+10FFFF.
    EXPECT_EQ(sanitizedUtf8("\xF4\x90\x80\x80"), withFffd("", 4, ""));
    // E0 80 80 is an overlong form.
    EXPECT_EQ(sanitizedUtf8("\xE0\x80\x80"), withFffd("", 3, ""));
}

TEST(Utf8SanitizeTest, ATruncatedSequenceIsOneReplacement)
{
    // E2 82 is the start of a three-byte character (E2 82 AC is the euro sign): one maximal subpart.
    EXPECT_EQ(sanitizedUtf8("price \xE2\x82"), withFffd("price ", 1, ""));
    EXPECT_EQ(sanitizedUtf8("\xE2\x82x"), withFffd("", 1, "x"));
    EXPECT_EQ(sanitizedUtf8("\xF0\x9F\x98"), withFffd("", 1, ""));
}

TEST(Utf8SanitizeTest, AStrayContinuationByteIsOneReplacementEach)
{
    EXPECT_EQ(sanitizedUtf8("\x80\xBF"), withFffd("", 2, ""));
}

TEST(Utf8SanitizeTest, TheResultIsAlwaysWellFormed)
{
    std::string every;
    for (int value = 0; value < 0x100; ++value)
    {
        every += static_cast<char>(value);
    }
    EXPECT_TRUE(isValidUtf8(sanitizedUtf8(every)));
}

// --- Through the single-line helpers the process snapshot uses ---

TEST(Utf8SanitizeTest, ToSingleLineReplacesInvalidBytesAndControls)
{
    std::string out;
    EXPECT_TRUE(toSingleLineInto(out, "Caf\xE9\n.txt"));
    EXPECT_EQ(out, withFffd("Caf", 1, " .txt"));
    EXPECT_FALSE(isSingleLine("Caf\xE9.txt"));
    EXPECT_TRUE(isSingleLine("café.txt"));
}

TEST(Utf8SanitizeTest, TheMemoShowsTheReplacementButKeepsNoCopyForValidText)
{
    SingleLineMemo memo;
    const std::string cut = std::string("QQ音乐播放器").substr(0, 15);
    memo.update(cut, false);
    EXPECT_EQ(memo.value(cut), withFffd("QQ音乐播放", 1, ""));
    memo.update("QQ音乐播放器", false);
    EXPECT_EQ(memo.value("QQ音乐播放器"), "QQ音乐播放器");
}

} // namespace
} // namespace Domain
