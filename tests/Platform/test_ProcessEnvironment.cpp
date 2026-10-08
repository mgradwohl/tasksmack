/// @file test_ProcessEnvironment.cpp
/// @brief The pure environment helpers (#179): parsing a NUL-separated environ block, escaping what a
/// process put in it so it is safe to draw, and mapping a read's errno to a status. Built on every
/// platform: no OS calls.

#include "Platform/IProcessEnvironment.h"
#include "Platform/ProcessEnvironment.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Environment
{
namespace
{

using namespace std::string_view_literals;

// ========== parseEnvironBlock ==========

TEST(ProcessEnvironmentParseTest, SplitsNulSeparatedEntries)
{
    const auto vars = parseEnvironBlock("HOME=/home/me\0PATH=/usr/bin:/bin\0LANG=C.UTF-8\0"sv);
    ASSERT_EQ(vars.size(), 3U);
    EXPECT_EQ(vars[0].name, "HOME");
    EXPECT_EQ(vars[0].value, "/home/me");
    EXPECT_EQ(vars[1].name, "PATH");
    EXPECT_EQ(vars[1].value, "/usr/bin:/bin");
    EXPECT_EQ(vars[2].name, "LANG");
    EXPECT_EQ(vars[2].value, "C.UTF-8");
}

TEST(ProcessEnvironmentParseTest, EmptyBlockHasNoEntries)
{
    EXPECT_TRUE(parseEnvironBlock(""sv).empty());
    EXPECT_TRUE(parseEnvironBlock("\0"sv).empty());
    EXPECT_TRUE(parseEnvironBlock("\0\0\0"sv).empty());
}

TEST(ProcessEnvironmentParseTest, LastEntryWithoutTrailingNulIsKept)
{
    const auto vars = parseEnvironBlock("A=1\0B=2"sv);
    ASSERT_EQ(vars.size(), 2U);
    EXPECT_EQ(vars[1].name, "B");
    EXPECT_EQ(vars[1].value, "2");
}

TEST(ProcessEnvironmentParseTest, DoubledNulsAreSkipped)
{
    const auto vars = parseEnvironBlock("A=1\0\0B=2\0"sv);
    ASSERT_EQ(vars.size(), 2U);
    EXPECT_EQ(vars[0].name, "A");
    EXPECT_EQ(vars[1].name, "B");
}

TEST(ProcessEnvironmentParseTest, EntryWithoutEqualsIsANameWithEmptyValue)
{
    const auto vars = parseEnvironBlock("JUSTANAME\0X=y\0"sv);
    ASSERT_EQ(vars.size(), 2U);
    EXPECT_EQ(vars[0].name, "JUSTANAME");
    EXPECT_EQ(vars[0].value, "");
    EXPECT_EQ(vars[1].name, "X");
}

TEST(ProcessEnvironmentParseTest, EmbeddedEqualsStayInTheValue)
{
    const auto vars = parseEnvironBlock("OPTS=--a=1 --b=2\0EMPTY=\0EQ===\0"sv);
    ASSERT_EQ(vars.size(), 3U);
    EXPECT_EQ(vars[0].name, "OPTS");
    EXPECT_EQ(vars[0].value, "--a=1 --b=2");
    EXPECT_EQ(vars[1].name, "EMPTY");
    EXPECT_EQ(vars[1].value, "");
    EXPECT_EQ(vars[2].name, "EQ");
    EXPECT_EQ(vars[2].value, "==");
}

TEST(ProcessEnvironmentParseTest, LeadingEqualsBelongsToTheName)
{
    const auto vars = parseEnvironBlock("=C:=C:\\dir\0"sv);
    ASSERT_EQ(vars.size(), 1U);
    EXPECT_EQ(vars[0].name, "=C:");
    EXPECT_EQ(vars[0].value, "C:\\dir");
}

TEST(ProcessEnvironmentParseTest, InvalidUtf8IsEscapedNotDropped)
{
    // 0xFF is never valid; 0xC3 alone is a truncated two-byte sequence.
    const auto vars = parseEnvironBlock("BAD=a\xFF"
                                        "b\0TRUNC=\xC3\0"sv);
    ASSERT_EQ(vars.size(), 2U);
    EXPECT_EQ(vars[0].value, "a\\xFFb");
    EXPECT_EQ(vars[1].value, "\\xC3");
}

TEST(ProcessEnvironmentParseTest, ControlCharactersAreEscaped)
{
    const auto vars = parseEnvironBlock("BASH_FUNC_f%%=() {  echo hi\n}\0TABBED=a\tb\r\x1B[0m\0"sv);
    ASSERT_EQ(vars.size(), 2U);
    EXPECT_EQ(vars[0].name, "BASH_FUNC_f%%");
    EXPECT_EQ(vars[0].value, "() {  echo hi\\n}");
    EXPECT_EQ(vars[1].value, "a\\tb\\r\\x1B[0m");
}

TEST(ProcessEnvironmentParseTest, LargeBinaryValueIsEscapedWhole)
{
    // 1 MiB of 0x01: every byte becomes the four characters "\x01".
    constexpr std::size_t VALUE_BYTES = std::size_t{1} << 20U;
    std::string block = "BIN=";
    block.append(VALUE_BYTES, '\x01');
    block.push_back('\0');
    const auto vars = parseEnvironBlock(block);
    ASSERT_EQ(vars.size(), 1U);
    EXPECT_EQ(vars[0].name, "BIN");
    ASSERT_EQ(vars[0].value.size(), VALUE_BYTES * 4U);
    EXPECT_EQ(vars[0].value.substr(0, 8), R"(\x01\x01)");
    EXPECT_EQ(vars[0].value.substr(vars[0].value.size() - 4), R"(\x01)");
}

// ========== escapeForDisplay ==========

TEST(ProcessEnvironmentEscapeTest, KeepsWellFormedUtf8)
{
    // 2-, 3- and 4-byte sequences: e-acute, the euro sign, and an emoji
    const std::string text = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80";
    EXPECT_EQ(escapeForDisplay(text), text);
}

TEST(ProcessEnvironmentEscapeTest, EscapesMalformedSequences)
{
    struct Case
    {
        std::string_view input;
        std::string_view expected;
    };
    const std::array<Case, 7> cases{{
        {.input = "\x80", .expected = R"(\x80)"},                         // stray continuation byte
        {.input = "\xC0\xAF", .expected = R"(\xC0\xAF)"},                 // overlong '/'
        {.input = "\xE0\x80\xAF", .expected = R"(\xE0\x80\xAF)"},         // overlong 3-byte
        {.input = "\xED\xA0\x80", .expected = R"(\xED\xA0\x80)"},         // UTF-16 surrogate
        {.input = "\xF4\x90\x80\x80", .expected = R"(\xF4\x90\x80\x80)"}, // past U+10FFFF
        {.input = "\xE2\x82", .expected = R"(\xE2\x82)"},                 // truncated
        {.input = "\x7F", .expected = R"(\x7F)"},                         // DEL
    }};
    for (const Case& c : cases)
    {
        SCOPED_TRACE(std::string(c.expected));
        EXPECT_EQ(escapeForDisplay(c.input), c.expected);
    }
}

// ========== statusFromErrno ==========

TEST(ProcessEnvironmentStatusTest, MapsReadErrorsToStatuses)
{
    struct Case
    {
        int err;
        EnvironmentReadStatus expected;
    };
    const std::array<Case, 6> cases{{
        {.err = EACCES, .expected = EnvironmentReadStatus::PermissionDenied},
        {.err = EPERM, .expected = EnvironmentReadStatus::PermissionDenied},
        {.err = ENOENT, .expected = EnvironmentReadStatus::ProcessExited},
        {.err = ESRCH, .expected = EnvironmentReadStatus::ProcessExited},
        {.err = EIO, .expected = EnvironmentReadStatus::Failed},
        {.err = 0, .expected = EnvironmentReadStatus::Failed},
    }};
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.err);
        EXPECT_EQ(statusFromErrno(c.err), c.expected);
    }
}

// ========== UnsupportedProcessEnvironmentReader ==========

TEST(ProcessEnvironmentUnsupportedTest, ReportsNoSupportAndNeverReads)
{
    UnsupportedProcessEnvironmentReader reader;
    EXPECT_FALSE(reader.hasEnvironment());
    const EnvironmentReadResult result = reader.readEnvironment({.pid = 1, .startTimeTicks = 1});
    EXPECT_EQ(result.status, EnvironmentReadStatus::Unsupported);
    EXPECT_TRUE(result.variables.empty());
}

} // namespace
} // namespace Platform::Environment
