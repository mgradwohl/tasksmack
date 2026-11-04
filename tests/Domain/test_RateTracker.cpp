/// @file test_RateTracker.cpp
/// @brief Domain::RateTracker and the counter helpers (#3344): intervals between readings by acquisition time,
/// with fixed timestamps and no clock.

#include "Domain/RateTracker.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>

namespace
{

using Domain::AcquiredAt;
using Domain::StartedIdentity;
using Tracker = Domain::RateTracker<StartedIdentity, std::uint64_t, Domain::StartedIdentityHash>;

[[nodiscard]] AcquiredAt at(double seconds)
{
    return Domain::acquiredAtFromSeconds(seconds);
}

TEST(RateTrackerTest, AFirstReadingGivesNoIntervalAndTheNextGivesTheTimeBetweenAcquisitions)
{
    Tracker tracker;
    const StartedIdentity thread{.id = 7, .startTicks = 100};
    EXPECT_FALSE(tracker.observe(thread, 1000, at(10.0)).has_value());
    const auto interval = tracker.observe(thread, 1500, at(12.0));
    ASSERT_TRUE(interval.has_value());
    EXPECT_EQ(interval->previous, 1000U);
    EXPECT_DOUBLE_EQ(interval->seconds, 2.0);
    ASSERT_NE(tracker.last(thread), nullptr);
    EXPECT_EQ(tracker.last(thread)->value, 1500U);
}

// #3337/#3344: the interval is between the readings' acquisitions, so when a reading is taken in (adopted) doesn't
// matter: one adopted late gives the same rate as one adopted at once.
TEST(RateTrackerTest, ADelayedAdoptionGivesTheSameRate)
{
    Tracker prompt;
    Tracker late;
    const StartedIdentity process{.id = 42, .startTicks = 5};
    static_cast<void>(prompt.observe(process, 0, at(100.0)));
    static_cast<void>(late.observe(process, 0, at(100.0)));
    // Both readings were acquired 1 s apart; the late one is observed whenever the UI gets to it, with the same times.
    const auto promptly = prompt.observe(process, 250, at(101.0));
    const auto adoptedLate = late.observe(process, 250, at(101.0));
    ASSERT_TRUE(promptly.has_value());
    ASSERT_TRUE(adoptedLate.has_value());
    EXPECT_DOUBLE_EQ(*Domain::counterRate(250, promptly->previous, promptly->seconds), 250.0);
    EXPECT_DOUBLE_EQ(*Domain::counterRate(250, adoptedLate->previous, adoptedLate->seconds), 250.0);
}

TEST(RateTrackerTest, TimeThatDoesNotMoveForwardGivesNoIntervalButTheReadingStillBecomesTheLast)
{
    Tracker tracker;
    const StartedIdentity thread{.id = 1, .startTicks = 0};
    static_cast<void>(tracker.observe(thread, 10, at(5.0)));
    EXPECT_FALSE(tracker.observe(thread, 20, at(5.0)).has_value()) << "zero elapsed: nothing to divide by";
    EXPECT_FALSE(tracker.observe(thread, 30, at(4.0)).has_value()) << "earlier than the last";
    EXPECT_EQ(tracker.last(thread)->value, 30U);
    const auto interval = tracker.observe(thread, 40, at(6.0));
    ASSERT_TRUE(interval.has_value());
    EXPECT_EQ(interval->previous, 30U);
    EXPECT_DOUBLE_EQ(interval->seconds, 2.0);
}

TEST(RateTrackerTest, AnIntervalNoLongerThanTheMinimumGivesNoRate)
{
    Tracker tracker(0.05);
    const StartedIdentity process{.id = 3, .startTicks = 9};
    static_cast<void>(tracker.observe(process, 0, at(1.0)));
    EXPECT_FALSE(tracker.observe(process, 5, at(1.05)).has_value()) << "at the minimum: a seed reading's tiny interval";
    const auto interval = tracker.observe(process, 105, at(2.05));
    ASSERT_TRUE(interval.has_value());
    EXPECT_EQ(interval->previous, 5U);
    EXPECT_DOUBLE_EQ(interval->seconds, 1.0);
}

// A reused number with a new start time is a different identity: its first reading gives no interval, and the old
// identity is forgotten when a round doesn't see it.
TEST(RateTrackerTest, AReusedNumberIsANewIdentityAndTheOldOneIsForgottenAfterARoundWithoutIt)
{
    Tracker tracker;
    const StartedIdentity first{.id = 500, .startTicks = 1000};
    const StartedIdentity reused{.id = 500, .startTicks = 2000};
    tracker.beginRound();
    static_cast<void>(tracker.observe(first, 900, at(1.0)));
    tracker.endRound();

    tracker.beginRound();
    EXPECT_FALSE(tracker.observe(reused, 10, at(2.0)).has_value()) << "the reused PID's first reading";
    tracker.endRound();
    EXPECT_EQ(tracker.size(), 1U);
    EXPECT_EQ(tracker.last(first), nullptr) << "the process that ended is gone";
    ASSERT_NE(tracker.last(reused), nullptr);
}

TEST(RateTrackerTest, ClearMakesEveryNextReadingAFirstOne)
{
    Tracker tracker;
    const StartedIdentity thread{.id = 2, .startTicks = 1};
    static_cast<void>(tracker.observe(thread, 1, at(1.0)));
    tracker.clear();
    EXPECT_EQ(tracker.size(), 0U);
    EXPECT_FALSE(tracker.observe(thread, 2, at(2.0)).has_value());
}

TEST(RateTrackerTest, ACounterThatWentBackGivesNoChangeAndNoRate)
{
    EXPECT_EQ(Domain::counterDelta(150, 100), std::optional<std::uint64_t>(50));
    EXPECT_EQ(Domain::counterDelta(100, 100), std::optional<std::uint64_t>(0));
    EXPECT_FALSE(Domain::counterDelta(5, 100).has_value()) << "a reset (or a wrap): not an 18-quintillion jump";
    EXPECT_DOUBLE_EQ(*Domain::counterRate(300, 100, 2.0), 100.0);
    EXPECT_FALSE(Domain::counterRate(5, 100, 2.0).has_value());
    EXPECT_FALSE(Domain::counterRate(300, 100, 0.0).has_value()) << "zero elapsed";
    EXPECT_FALSE(Domain::counterRate(300, 100, -1.0).has_value());
}

TEST(RateTrackerTest, IdentitiesHashAndCompareOnBothParts)
{
    const Domain::StartedIdentityHash hash;
    const StartedIdentity a{.id = 10, .startTicks = 1};
    const StartedIdentity b{.id = 10, .startTicks = 2};
    EXPECT_NE(a, b);
    EXPECT_EQ(a, (StartedIdentity{.id = 10, .startTicks = 1}));
    EXPECT_NE(hash(a), hash(b)) << "the start time takes part in the hash";
    EXPECT_EQ(hash(a), hash(StartedIdentity{.id = 10, .startTicks = 1}));
}

TEST(RateTrackerTest, AcquiredAtFromSecondsKeepsTheIntervalBetweenTwoTimes)
{
    EXPECT_DOUBLE_EQ(std::chrono::duration<double>(at(12345.25) - at(12344.75)).count(), 0.5);
}

} // namespace
