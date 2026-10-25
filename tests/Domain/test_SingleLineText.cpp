#include "Domain/SingleLineText.h"

#include <gtest/gtest.h>

#include <string>

namespace Domain
{
namespace
{

TEST(SingleLineTextTest, OrdinaryTextIsUnchanged)
{
    EXPECT_EQ(toSingleLine("/usr/bin/bash --login"), "/usr/bin/bash --login");
    EXPECT_EQ(toSingleLine(""), "");
}

TEST(SingleLineTextTest, NewlineBecomesSpace)
{
    // The reported case (#919): a bash -c heredoc puts a literal newline inside one argv entry,
    // which ImGui would otherwise draw as multi-line text and grow the table row.
    EXPECT_EQ(toSingleLine("bash -c printf \"a\nb\""), "bash -c printf \"a b\"");
}

TEST(SingleLineTextTest, CarriageReturnAndTabBecomeSpaces)
{
    EXPECT_EQ(toSingleLine("a\r\nb\tc"), "a  b c");
}

TEST(SingleLineTextTest, AllC0ControlsAndDelAreReplaced)
{
    std::string input;
    for (int c = 0; c < 0x20; ++c)
    {
        input.push_back(static_cast<char>(c));
    }
    input.push_back(static_cast<char>(0x7F));

    const std::string out = toSingleLine(input);

    ASSERT_EQ(out.size(), input.size()); // One-for-one replacement, never a length change.
    for (const char c : out)
    {
        EXPECT_EQ(c, ' ');
    }
}

TEST(SingleLineTextTest, PrintableBoundariesAreKept)
{
    // 0x20 (space) and 0x7E (~) sit either side of the replaced range and must survive.
    EXPECT_EQ(toSingleLine(" ~"), " ~");
}

TEST(SingleLineTextTest, Utf8SequencesSurviveIntact)
{
    // Continuation bytes are >= 0x80 and must not be touched, or multi-byte characters would be
    // corrupted into mojibake by a naive "replace anything unusual" rule.
    const std::string utf8 = "café — ünïcödé ✓";
    EXPECT_EQ(toSingleLine(utf8), utf8);
}

TEST(SingleLineTextTest, EmbeddedNulBecomesSpace)
{
    const std::string input("a\0b", 3);
    EXPECT_EQ(toSingleLine(input), "a b");
}

// --- The fast path (#1624) ---

TEST(SingleLineTextTest, IsSingleLineMatchesEveryByte)
{
    // isSingleLine() must agree with toSingleLine() on every byte value, or a string it passes would
    // keep a control character.
    for (int value = 0; value < 0x100; ++value)
    {
        const std::string one(1, static_cast<char>(value));
        EXPECT_EQ(isSingleLine(one), toSingleLine(one) == one) << "byte " << value;
    }
}

TEST(SingleLineTextTest, IsSingleLineOnTypicalText)
{
    EXPECT_TRUE(isSingleLine(""));
    EXPECT_TRUE(isSingleLine("/usr/bin/bash --login"));
    EXPECT_TRUE(isSingleLine("café — ünïcödé ✓"));
    EXPECT_FALSE(isSingleLine("a\nb"));
    EXPECT_FALSE(isSingleLine("a\rb"));
    EXPECT_FALSE(isSingleLine("a\tb"));
    EXPECT_FALSE(isSingleLine(std::string("a\0b", 3)));
    EXPECT_FALSE(isSingleLine("a\x1B[31mb"));
    EXPECT_FALSE(isSingleLine("del\x7F"));
    // A control at the very end of a long string, past any vectorized block boundary.
    EXPECT_FALSE(isSingleLine(std::string(1000, 'x') + '\n'));
}

TEST(SingleLineTextTest, ToSingleLineIntoReportsWhetherItChanged)
{
    std::string out;
    EXPECT_FALSE(toSingleLineInto(out, "/usr/bin/bash --login"));
    EXPECT_EQ(out, "/usr/bin/bash --login");

    EXPECT_FALSE(toSingleLineInto(out, "café — ünïcödé ✓"));
    EXPECT_EQ(out, "café — ünïcödé ✓");

    EXPECT_TRUE(toSingleLineInto(out, "a\r\nb\tc"));
    EXPECT_EQ(out, "a  b c");

    EXPECT_TRUE(toSingleLineInto(out, "\x01\x1F\x7F"));
    EXPECT_EQ(out, "   ");

    EXPECT_FALSE(toSingleLineInto(out, ""));
    EXPECT_EQ(out, "");
}

TEST(SingleLineTextTest, ToSingleLineIntoReusesTheDestination)
{
    std::string out(64, 'z');
    out.clear();
    const auto* const buffer = out.data();
    EXPECT_FALSE(toSingleLineInto(out, "a string longer than any small-string buffer"));
    EXPECT_EQ(out, "a string longer than any small-string buffer");
    EXPECT_EQ(out.data(), buffer); // Fitted in the capacity it had: no new allocation.
}

TEST(SingleLineTextTest, MemoDerivesOnlyWhenTheRawStringChanged)
{
    SingleLineMemo memo;

    EXPECT_TRUE(memo.update("plain", false));
    EXPECT_EQ(memo.value("plain"), "plain");

    EXPECT_FALSE(memo.update("plain", true)); // Unchanged: not derived again.
    EXPECT_EQ(memo.value("plain"), "plain");

    EXPECT_TRUE(memo.update("two\nlines", false)); // Changed: derived again, and sanitized.
    EXPECT_EQ(memo.value("two\nlines"), "two lines");

    EXPECT_FALSE(memo.update("two\nlines", true)); // The kept sanitized form.
    EXPECT_EQ(memo.value("two\nlines"), "two lines");

    EXPECT_TRUE(memo.update("ünï\tcödé", false));
    EXPECT_EQ(memo.value("ünï\tcödé"), "ünï cödé");

    EXPECT_TRUE(memo.update("", false)); // Back to a clean string: the old sanitized form is dropped.
    EXPECT_EQ(memo.value(""), "");
}

} // namespace
} // namespace Domain
