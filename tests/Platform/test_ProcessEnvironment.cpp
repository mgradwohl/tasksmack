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

// #4328: the reader keeps the raw bytes; the UI makes display text (Domain::DisplayText, tested on its own).
TEST(ProcessEnvironmentParseTest, InvalidUtf8AndControlsAreKeptRaw)
{
    const auto vars = parseEnvironBlock("BAD=a\xFF"
                                        "b\0BASH_FUNC_f%%=() {  echo hi\n}\0TABBED=a\tb\r\x1B[0m\0"sv);
    ASSERT_EQ(vars.size(), 3U);
    EXPECT_EQ(vars[0].value,
              "a\xFF"
              "b");
    EXPECT_EQ(vars[1].name, "BASH_FUNC_f%%");
    EXPECT_EQ(vars[1].value, "() {  echo hi\n}");
    EXPECT_EQ(vars[2].value, "a\tb\r\x1B[0m");
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
