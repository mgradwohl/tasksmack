/// @file test_ProcessIdentity.cpp
/// @brief Tests for the pure identity pieces of #973: Platform::checkProcessIdentity() and
/// Platform::ProcStat::parseStartTime(). Both are free of platform APIs, so they run on every host.

#include "Platform/IProcessActions.h"
#include "Platform/Linux/ProcStatStartTime.h"

#include <gtest/gtest.h>

#include <string>

namespace Platform
{
namespace
{

// =============================================================================
// checkProcessIdentity
// =============================================================================

TEST(ProcessIdentityTest, MatchingStartTimeIsAccepted)
{
    const auto result = checkProcessIdentity({.pid = 4242, .startTimeTicks = 133'000'000'000ULL}, 133'000'000'000ULL);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.errorMessage.empty());
}

TEST(ProcessIdentityTest, DifferentStartTimeIsRefusedAsAReusedPid)
{
    const auto result = checkProcessIdentity({.pid = 4242, .startTimeTicks = 1000}, 1001);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.errorMessage.find("4242"), std::string::npos) << result.errorMessage;
    EXPECT_NE(result.errorMessage.find("different process"), std::string::npos) << result.errorMessage;
}

TEST(ProcessIdentityTest, UnknownExpectedStartTimeIsRefused)
{
    // Skipping the check when the expected value is unknown would reopen the PID-only path, so an
    // unknown target is refused even when the process it finds also reports no start time.
    const auto againstZero = checkProcessIdentity({.pid = 4, .startTimeTicks = 0}, 0);
    const auto againstReal = checkProcessIdentity({.pid = 4242, .startTimeTicks = 0}, 1000);

    EXPECT_FALSE(againstZero.success);
    EXPECT_FALSE(againstReal.success);
    EXPECT_NE(againstReal.errorMessage.find("Cannot confirm"), std::string::npos) << againstReal.errorMessage;
}

// =============================================================================
// ProcStat::parseStartTime
// =============================================================================

// A real /proc/[pid]/stat line, trimmed after vsize; starttime (field 22) is 98765.
constexpr const char* STAT_LINE = "1234 (bash) S 1 1234 1234 34816 1234 4194560 1000 2000 0 0 10 5 0 0 20 0 1 0 98765 12345678 400\n";

TEST(ProcStatStartTimeTest, ReadsField22)
{
    EXPECT_EQ(ProcStat::parseStartTime(STAT_LINE), 98765ULL);
}

TEST(ProcStatStartTimeTest, NameWithSpacesAndParenthesesDoesNotShiftTheField)
{
    // comm is chosen by the process, so it can look like more fields. Counting from the last ")"
    // keeps a crafted name from steering which number is compared.
    const std::string line = "1234 (evil) 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 (x) S 1 1234 1234 34816 1234 4194560 "
                             "1000 2000 0 0 10 5 0 0 20 0 1 0 98765 12345678 400";

    EXPECT_EQ(ProcStat::parseStartTime(line), 98765ULL);
}

TEST(ProcStatStartTimeTest, LastFieldWithoutTrailingNewline)
{
    EXPECT_EQ(ProcStat::parseStartTime("1 (init) S 0 1 1 0 -1 4194560 1 2 3 4 5 6 7 8 20 0 1 0 42"), 42ULL);
}

TEST(ProcStatStartTimeTest, LargeStartTime)
{
    EXPECT_EQ(ProcStat::parseStartTime("1 (a) S 0 1 1 0 -1 4194560 1 2 3 4 5 6 7 8 20 0 1 0 18446744073709551615 9"),
              18'446'744'073'709'551'615ULL);
}

TEST(ProcStatStartTimeTest, MalformedLinesAreRejected)
{
    EXPECT_FALSE(ProcStat::parseStartTime("").has_value());
    EXPECT_FALSE(ProcStat::parseStartTime("1234 bash S 1 2 3").has_value());                            // no ")"
    EXPECT_FALSE(ProcStat::parseStartTime("1234 (bash) S 1 1234 1234 34816 1234 4194560").has_value()); // too short
    EXPECT_FALSE(ProcStat::parseStartTime("1 (a) S 0 1 1 0 -1 4194560 1 2 3 4 5 6 7 8 20 0 1 0 12x 9").has_value());
    EXPECT_FALSE(ProcStat::parseStartTime("1 (a) S 0 1 1 0 -1 4194560 1 2 3 4 5 6 7 8 20 0 1 0 -5 9").has_value());
}

} // namespace
} // namespace Platform
