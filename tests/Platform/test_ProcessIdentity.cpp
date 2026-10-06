/// @file test_ProcessIdentity.cpp
/// @brief Tests for the pure identity pieces of #973: Platform::checkProcessIdentity() and
/// the /proc/[pid]/stat parser it reads the start time with (Platform::ProcParsing::parseStatStartTime(),
/// and parseStatFields(), which LinuxProcessProbe reads the same field with, #1183). All are free of
/// platform APIs, so they run on every host.

#include "Platform/IProcessActions.h"
#include "Platform/Linux/ProcParsing.h"

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
// ProcParsing::parseStatStartTime
// =============================================================================

// A real /proc/[pid]/stat line, trimmed after vsize; starttime (field 22) is 98765.
constexpr const char* STAT_LINE = "1234 (bash) S 1 1234 1234 34816 1234 4194560 1000 2000 0 0 10 5 0 0 20 0 1 0 98765 12345678 400\n";

TEST(ProcStatStartTimeTest, ReadsField22)
{
    EXPECT_EQ(ProcParsing::parseStatStartTime(STAT_LINE), 98765ULL);
}

TEST(ProcStatStartTimeTest, NameWithSpacesAndParenthesesDoesNotShiftTheField)
{
    // comm is chosen by the process, so it can look like more fields. Counting from the last ")"
    // keeps a crafted name from steering which number is compared.
    const std::string line = "1234 (evil) 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 (x) S 1 1234 1234 34816 1234 4194560 "
                             "1000 2000 0 0 10 5 0 0 20 0 1 0 98765 12345678 400";

    EXPECT_EQ(ProcParsing::parseStatStartTime(line), 98765ULL);
}

TEST(ProcStatStartTimeTest, LastFieldWithoutTrailingNewline)
{
    EXPECT_EQ(ProcParsing::parseStatStartTime("1 (init) S 0 1 1 0 -1 4194560 1 2 3 4 5 6 7 8 20 0 1 0 42"), 42ULL);
}

TEST(ProcStatStartTimeTest, LargeStartTime)
{
    EXPECT_EQ(ProcParsing::parseStatStartTime("1 (a) S 0 1 1 0 -1 4194560 1 2 3 4 5 6 7 8 20 0 1 0 18446744073709551615 9"),
              18'446'744'073'709'551'615ULL);
}

TEST(ProcStatStartTimeTest, MalformedLinesAreRejected)
{
    EXPECT_FALSE(ProcParsing::parseStatStartTime("").has_value());
    EXPECT_FALSE(ProcParsing::parseStatStartTime("1234 bash S 1 2 3").has_value());                            // no ")"
    EXPECT_FALSE(ProcParsing::parseStatStartTime("1234 (bash) S 1 1234 1234 34816 1234 4194560").has_value()); // too short
    EXPECT_FALSE(ProcParsing::parseStatStartTime("1 (a) S 0 1 1 0 -1 4194560 1 2 3 4 5 6 7 8 20 0 1 0 12x 9").has_value());
    EXPECT_FALSE(ProcParsing::parseStatStartTime("1 (a) S 0 1 1 0 -1 4194560 1 2 3 4 5 6 7 8 20 0 1 0 -5 9").has_value());
}

// =============================================================================
// ProcParsing::parseStatFields -- the probe's fields, counted the same way (#1183)
// =============================================================================

// A full kernel line (52 fields): minflt 1000, majflt 0, utime 10, stime 5, nice -5, 3 threads,
// starttime 98765, vsize 12345678, rss 400 pages.
constexpr const char* FULL_STAT_LINE =
    "1234 (my (odd) name) S 1 1234 1234 34816 1234 4194560 1000 2000 0 0 10 5 0 0 20 -5 3 0 98765 12345678 400 "
    "18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 17 3 0 0 0 0 0 0 0 0 0 0 0 0 0\n";

TEST(ProcStatFieldsTest, ReadsEveryFieldTheProbeUses)
{
    const auto parsed = ProcParsing::parseStatFields(FULL_STAT_LINE);
    ASSERT_TRUE(parsed.has_value());
    const ProcParsing::StatFields fields = parsed.value_or(ProcParsing::StatFields{});
    EXPECT_EQ(fields.comm, "my (odd) name"); // first '(' to last ')'
    EXPECT_EQ(fields.state, 'S');
    EXPECT_EQ(fields.parentPid, 1);
    EXPECT_EQ(fields.minorFaults, 1000U);
    EXPECT_EQ(fields.majorFaults, 0U);
    EXPECT_EQ(fields.userTime, 10U);
    EXPECT_EQ(fields.systemTime, 5U);
    EXPECT_EQ(fields.nice, -5);
    EXPECT_EQ(fields.numThreads, 3);
    EXPECT_EQ(fields.startTime, 98765U);
    EXPECT_EQ(fields.virtualBytes, 12345678U);
    EXPECT_EQ(fields.rssPages, 400);
}

TEST(ProcStatFieldsTest, StartTimeAgreesWithTheStartTimeParser)
{
    // The identity check (#973) and the probe's startTimeTicks must never read different fields.
    for (const char* line : {FULL_STAT_LINE, STAT_LINE})
    {
        SCOPED_TRACE(line);
        const auto fields = ProcParsing::parseStatFields(line);
        ASSERT_TRUE(fields.has_value());
        EXPECT_EQ(ProcParsing::parseStatStartTime(line), fields.value_or(ProcParsing::StatFields{}).startTime);
    }
}

TEST(ProcStatFieldsTest, LineEndingBeforeRssIsRejectedButStillHasAStartTime)
{
    // The probe needs vsize and rss; the identity check needs nothing after the start time.
    constexpr const char* THROUGH_START_TIME = "1 (init) S 0 1 1 0 -1 4194560 1 2 3 4 5 6 7 8 20 0 1 0 42";
    EXPECT_FALSE(ProcParsing::parseStatFields(THROUGH_START_TIME).has_value());
    EXPECT_EQ(ProcParsing::parseStatStartTime(THROUGH_START_TIME), 42ULL);
}

TEST(ProcStatFieldsTest, MalformedLinesAreRejected)
{
    EXPECT_FALSE(ProcParsing::parseStatFields("").has_value());
    EXPECT_FALSE(ProcParsing::parseStatFields("1234 bash) S 1 1234").has_value()); // no "("
    EXPECT_FALSE(ProcParsing::parseStatFields("1234 (bash) ").has_value());        // no state
    // A non-numeric field anywhere before rss.
    EXPECT_FALSE(ProcParsing::parseStatFields("1 (a) S 0 1 1 0 -1 4194560 1 2 3 x 5 6 7 8 20 0 1 0 42 9 9").has_value());
}

TEST(ProcStatFieldsTest, EveryFieldMustEndAtASeparator)
{
    // Each malformed line keeps the field count of the valid one, so only the token boundary rejects it.
    constexpr const char* VALID = "1 (a) S 0 1 1 0 -1 4194560 1 2 3 4 5 6 7 8 20 0 1 0 42 9 9";
    const auto valid = ProcParsing::parseStatFields(VALID);
    ASSERT_TRUE(valid.has_value());
    EXPECT_EQ(valid.value_or(ProcParsing::StatFields{}).startTime, 42U);
    EXPECT_EQ(valid.value_or(ProcParsing::StatFields{}).rssPages, 9);
    EXPECT_EQ(ProcParsing::parseStatStartTime(VALID), 42ULL);

    // "7-8" in place of cutime/cstime "7 8" is not the two fields 7 and -8, for either entry point.
    constexpr const char* JOINED_CHILD_TIMES = "1 (a) S 0 1 1 0 -1 4194560 1 2 3 4 5 6 7-8 20 0 1 0 42 9 9";
    EXPECT_FALSE(ProcParsing::parseStatFields(JOINED_CHILD_TIMES).has_value());
    EXPECT_FALSE(ProcParsing::parseStatStartTime(JOINED_CHILD_TIMES).has_value());

    // A unit-like suffix on a field before the start time.
    constexpr const char* SUFFIXED_FLAGS = "1 (a) S 0 1 1 0 -1 4194560kB 1 2 3 4 5 6 7 8 20 0 1 0 42 9 9";
    EXPECT_FALSE(ProcParsing::parseStatFields(SUFFIXED_FLAGS).has_value());
    EXPECT_FALSE(ProcParsing::parseStatStartTime(SUFFIXED_FLAGS).has_value());

    // A trailing "9x" as rss is not 9; the start time, read before it, is unaffected.
    constexpr const char* SUFFIXED_RSS = "1 (a) S 0 1 1 0 -1 4194560 1 2 3 4 5 6 7 8 20 0 1 0 42 9 9x";
    EXPECT_FALSE(ProcParsing::parseStatFields(SUFFIXED_RSS).has_value());
    EXPECT_EQ(ProcParsing::parseStatStartTime(SUFFIXED_RSS), 42ULL);
    EXPECT_FALSE(ProcParsing::parseStatFields("1 (a) S 0 1 1 0 -1 4194560 1 2 3 4 5 6 7 8 20 0 1 0 42 9 9x\n").has_value());
}

TEST(ProcStatFieldsTest, StateMustBeFollowedByASeparator)
{
    // "S0" is not the state S followed by ppid 0.
    constexpr const char* JOINED_STATE = "1 (a) S0 1 1 0 -1 4194560 1 2 3 4 5 6 7 8 20 0 1 0 42 9 9";
    EXPECT_FALSE(ProcParsing::parseStatFields(JOINED_STATE).has_value());
    EXPECT_FALSE(ProcParsing::parseStatStartTime(JOINED_STATE).has_value());
    EXPECT_FALSE(ProcParsing::parseStatFields("1 (a) S").has_value()); // state ends the line
}

} // namespace
} // namespace Platform
