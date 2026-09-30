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

} // namespace
} // namespace Domain
