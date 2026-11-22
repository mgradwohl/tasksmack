// Tests for Domain::KeyedLookup (#3718): a lookup made once per key on a worker thread of its own, taken by a
// later find().

#include "Domain/KeyedLookup.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace Domain
{
namespace
{

constexpr std::string_view THREAD_NAME = "ts-test-lookup";
constexpr std::chrono::milliseconds LOOKUP_WAIT{5000};

/// What the fake lookup was asked, and where.
struct Calls
{
    std::mutex mutex;
    std::map<std::string, int> byKey;
    std::thread::id thread;
    bool gateOpen = true;    ///< Closed: a lookup waits until it opens
    bool emptyNamed = false; ///< Whether "empty" has a name yet; until then it answers ""
    int entered = 0;         ///< Lookups begun, gate or not
    std::condition_variable gateChanged;
};

Calls& calls()
{
    static Calls state;
    return state;
}

/// The lookups' clock in the retry tests, moved by hand; read on the worker's thread.
std::atomic<std::chrono::steady_clock::rep>& fakeTicks()
{
    static std::atomic<std::chrono::steady_clock::rep> ticks{0};
    return ticks;
}

std::chrono::steady_clock::time_point fakeNow()
{
    return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(fakeTicks().load()));
}

void advanceClock(std::chrono::steady_clock::duration by)
{
    fakeTicks() += by.count();
}

constexpr std::chrono::milliseconds FIRST_RETRY{30'000};
constexpr std::chrono::milliseconds LATER_RETRY{300'000};

std::atomic<int>& drainsBegun()
{
    static std::atomic<int> count{0};
    return count;
}

std::atomic<int>& drainsEnded()
{
    static std::atomic<int> count{0};
    return count;
}

void beginDrain()
{
    ++drainsBegun();
}

void endDrain()
{
    ++drainsEnded();
}

std::string lookUp(const std::string& key)
{
    std::unique_lock lock(calls().mutex);
    ++calls().entered;
    calls().gateChanged.notify_all();
    calls().gateChanged.wait(lock, [] { return calls().gateOpen; });
    ++calls().byKey[key];
    calls().thread = std::this_thread::get_id();
    if (key == "throws")
    {
        throw std::runtime_error("lookup failed");
    }
    if (key == "empty" && !calls().emptyNamed)
    {
        return {};
    }
    return "name of " + key;
}

void setGate(bool open)
{
    {
        const std::scoped_lock lock(calls().mutex);
        calls().gateOpen = open;
    }
    calls().gateChanged.notify_all();
}

class KeyedLookupTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const std::scoped_lock lock(calls().mutex);
        calls().byKey.clear();
        calls().thread = {};
        calls().gateOpen = true;
        calls().entered = 0;
        calls().emptyNamed = false;
        fakeTicks() = 0;
        drainsBegun() = 0;
        drainsEnded() = 0;
    }
    void TearDown() override
    {
        setGate(true); // Never leave a worker blocked in a lookup
    }

    [[nodiscard]] static int lookupsOf(const std::string& key)
    {
        const std::scoped_lock lock(calls().mutex);
        const auto found = calls().byKey.find(key);
        return found != calls().byKey.end() ? found->second : 0;
    }
};

TEST_F(KeyedLookupTest, AKeyIsLookedUpOnTheWorkersThreadAndTakenByALaterFind)
{
    KeyedLookup<std::string> lookup({.threadName = THREAD_NAME, .beginDrain = &beginDrain, .endDrain = &endDrain}, &lookUp);
    EXPECT_FALSE(lookup.find("App.1").has_value()) << "queued, not looked up here";
    ASSERT_TRUE(lookup.waitUntilIdle(LOOKUP_WAIT));
    EXPECT_EQ(lookup.find("App.1"), std::optional<std::string>("name of App.1"));
    {
        const std::scoped_lock lock(calls().mutex);
        EXPECT_NE(calls().thread, std::this_thread::get_id());
    }
    EXPECT_GE(drainsBegun().load(), 1);
    // Idle comes as the queue empties; the pass's end hook runs just after, on the worker's thread.
    const auto deadline = std::chrono::steady_clock::now() + LOOKUP_WAIT;
    while (drainsEnded().load() != drainsBegun().load() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(drainsBegun().load(), drainsEnded().load()) << "each pass over the queue ends what it began";
}

TEST_F(KeyedLookupTest, AKeyIsLookedUpOnceHoweverOftenItIsAskedFor)
{
    setGate(false);
    KeyedLookup<std::string> lookup({.threadName = THREAD_NAME}, &lookUp);
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_FALSE(lookup.find("App.1").has_value());
    }
    setGate(true);
    ASSERT_TRUE(lookup.waitUntilIdle(LOOKUP_WAIT));
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(lookup.find("App.1").has_value());
    }
    EXPECT_EQ(lookupsOf("App.1"), 1);
}

/// A lookup with the shipped retry schedule (two retries: FIRST_RETRY, then LATER_RETRY) on the fake clock.
KeyedLookup<std::string>::Options retryOptions(std::size_t maxEntries = 512)
{
    return {
        .threadName = THREAD_NAME,
        .maxEntries = maxEntries,
        .maxRetries = 2,
        .firstRetry = FIRST_RETRY,
        .laterRetry = LATER_RETRY,
        .now = &fakeNow,
    };
}

/// find() until @p key's lookup in progress, if any, has been answered; that answer.
std::optional<std::string> findSettled(KeyedLookup<std::string>& lookup, const std::string& key)
{
    (void) lookup.find(key);
    EXPECT_TRUE(lookup.waitUntilIdle(LOOKUP_WAIT));
    return lookup.find(key);
}

// #3794: a lookup that threw keeps no answer. find() answers nullopt, as while it waits, and the key is asked
// again after the first retry interval, then after the later one; out of retries, it answers none for good.
TEST_F(KeyedLookupTest, ALookupThatThrowsIsAskedAgainTwiceThenAnswersNone)
{
    KeyedLookup<std::string> lookup(retryOptions(), &lookUp);
    EXPECT_EQ(findSettled(lookup, "throws"), std::nullopt) << "no answer kept";
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_EQ(lookup.find("throws"), std::nullopt);
    }
    advanceClock(FIRST_RETRY - std::chrono::milliseconds(1));
    EXPECT_EQ(findSettled(lookup, "throws"), std::nullopt);
    EXPECT_EQ(lookupsOf("throws"), 1) << "not asked again before the first retry";

    advanceClock(std::chrono::milliseconds(1));
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_EQ(lookup.find("throws"), std::nullopt);
    }
    ASSERT_TRUE(lookup.waitUntilIdle(LOOKUP_WAIT));
    EXPECT_EQ(lookupsOf("throws"), 2) << "asked once more, not once a find()";

    advanceClock(FIRST_RETRY);
    EXPECT_EQ(findSettled(lookup, "throws"), std::nullopt);
    EXPECT_EQ(lookupsOf("throws"), 2) << "the second retry waits the later interval";
    advanceClock(LATER_RETRY - FIRST_RETRY);
    EXPECT_EQ(findSettled(lookup, "throws"), std::optional<std::string>(std::string{})) << "out of retries: none, kept";
    EXPECT_EQ(lookupsOf("throws"), 3);

    advanceClock(LATER_RETRY * 100);
    EXPECT_EQ(findSettled(lookup, "throws"), std::optional<std::string>(std::string{}));
    EXPECT_EQ(lookupsOf("throws"), 3) << "no retry after the last";
}

// #3794: an empty answer, the usual one (most processes have no Start-menu entry), is kept and answered, and
// asked again at most twice: after the first retry interval and the later one. Then it is kept for the session.
TEST_F(KeyedLookupTest, AnEmptyAnswerIsAskedAgainAtMostTwice)
{
    KeyedLookup<std::string> lookup(retryOptions(), &lookUp);
    EXPECT_EQ(findSettled(lookup, "empty"), std::optional<std::string>(std::string{}));
    advanceClock(FIRST_RETRY / 2);
    EXPECT_EQ(findSettled(lookup, "empty"), std::optional<std::string>(std::string{}));
    EXPECT_EQ(lookupsOf("empty"), 1) << "not asked again before the first retry";

    advanceClock(FIRST_RETRY);
    setGate(false);
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_FALSE(lookup.find("empty").has_value()) << "queued again, once";
    }
    setGate(true);
    ASSERT_TRUE(lookup.waitUntilIdle(LOOKUP_WAIT));
    EXPECT_EQ(lookup.find("empty"), std::optional<std::string>(std::string{}));
    EXPECT_EQ(lookupsOf("empty"), 2);

    advanceClock(LATER_RETRY);
    EXPECT_EQ(findSettled(lookup, "empty"), std::optional<std::string>(std::string{}));
    EXPECT_EQ(lookupsOf("empty"), 3) << "the second and last retry";

    for (int hour = 0; hour < 24; ++hour)
    {
        advanceClock(std::chrono::hours(1));
        EXPECT_EQ(findSettled(lookup, "empty"), std::optional<std::string>(std::string{}));
    }
    EXPECT_EQ(lookupsOf("empty"), 3) << "no third retry: the empty answer is kept for the session";
}

// #3794: a name that appears by a retry (the shell's Apps folder not ready at sign-in) is found, and kept.
TEST_F(KeyedLookupTest, AnAnswerFoundOnARetryIsKept)
{
    KeyedLookup<std::string> lookup(retryOptions(), &lookUp);
    EXPECT_EQ(findSettled(lookup, "empty"), std::optional<std::string>(std::string{}));
    {
        const std::scoped_lock lock(calls().mutex);
        calls().emptyNamed = true; // The shell has it now
    }
    advanceClock(FIRST_RETRY);
    EXPECT_EQ(findSettled(lookup, "empty"), std::optional<std::string>("name of empty"));
    EXPECT_EQ(lookupsOf("empty"), 2);

    advanceClock(LATER_RETRY * 100);
    EXPECT_EQ(findSettled(lookup, "empty"), std::optional<std::string>("name of empty"));
    EXPECT_EQ(lookupsOf("empty"), 2) << "a name is kept, not asked again";
}

// With no retries a key's first answer, empty or failed, is kept for good.
TEST_F(KeyedLookupTest, WithNoRetriesTheFirstAnswerIsKept)
{
    KeyedLookup<std::string>::Options options = retryOptions();
    options.maxRetries = 0;
    KeyedLookup<std::string> lookup(options, &lookUp);
    EXPECT_EQ(findSettled(lookup, "throws"), std::optional<std::string>(std::string{}));
    EXPECT_EQ(findSettled(lookup, "empty"), std::optional<std::string>(std::string{}));
    advanceClock(LATER_RETRY * 100);
    EXPECT_EQ(findSettled(lookup, "throws"), std::optional<std::string>(std::string{}));
    EXPECT_EQ(findSettled(lookup, "empty"), std::optional<std::string>(std::string{}));
    EXPECT_EQ(lookupsOf("throws"), 1);
    EXPECT_EQ(lookupsOf("empty"), 1);
}

// A full table forgets an empty or failed answer too, but never a key still waiting.
TEST_F(KeyedLookupTest, AFullTableForgetsAnEmptyOrFailedAnswer)
{
    KeyedLookup<std::string> lookup(retryOptions(2), &lookUp);
    for (const std::string key : {"empty", "throws"})
    {
        (void) lookup.find(key);
        ASSERT_TRUE(lookup.waitUntilIdle(LOOKUP_WAIT));
    }
    EXPECT_EQ(lookup.size(), 2U);
    EXPECT_FALSE(lookup.find("A").has_value()) << "room made";
    ASSERT_TRUE(lookup.waitUntilIdle(LOOKUP_WAIT));
    EXPECT_EQ(lookup.find("A"), std::optional<std::string>("name of A"));
    EXPECT_LE(lookup.size(), 2U);
}

TEST_F(KeyedLookupTest, AFullQueueTurnsAKeyAwayAndALaterFindAsksAgain)
{
    setGate(false);
    KeyedLookup<std::string> lookup({.threadName = THREAD_NAME, .maxQueued = 1}, &lookUp);
    (void) lookup.find("A");
    {
        std::unique_lock lock(calls().mutex); // "A" is taken off the queue into its (held) lookup
        ASSERT_TRUE(calls().gateChanged.wait_for(lock, LOOKUP_WAIT, [] { return calls().entered == 1; }));
    }
    EXPECT_FALSE(lookup.find("B").has_value()) << "queued: the one place in the queue";
    EXPECT_FALSE(lookup.find("C").has_value()) << "turned away";
    EXPECT_EQ(lookup.size(), 2U) << "A in progress and B waiting; C isn't held";
    setGate(true);
    ASSERT_TRUE(lookup.waitUntilIdle(LOOKUP_WAIT));
    EXPECT_FALSE(lookup.find("C").has_value()) << "asked again: queued now";
    ASSERT_TRUE(lookup.waitUntilIdle(LOOKUP_WAIT));
    EXPECT_EQ(lookup.find("C"), std::optional<std::string>("name of C"));
    EXPECT_EQ(lookupsOf("C"), 1);
}

TEST_F(KeyedLookupTest, AFullTableForgetsWhatWasAnsweredAndAsksAgain)
{
    KeyedLookup<std::string> lookup({.threadName = THREAD_NAME, .maxEntries = 2}, &lookUp);
    for (const std::string key : {"A", "B"})
    {
        (void) lookup.find(key);
        ASSERT_TRUE(lookup.waitUntilIdle(LOOKUP_WAIT));
        EXPECT_TRUE(lookup.find(key).has_value());
    }
    EXPECT_FALSE(lookup.find("C").has_value()) << "the answers taken are forgotten to make room";
    ASSERT_TRUE(lookup.waitUntilIdle(LOOKUP_WAIT));
    EXPECT_TRUE(lookup.find("C").has_value());
    EXPECT_LE(lookup.size(), 2U);
    EXPECT_FALSE(lookup.find("A").has_value()) << "forgotten: asked again";
    ASSERT_TRUE(lookup.waitUntilIdle(LOOKUP_WAIT));
    EXPECT_EQ(lookupsOf("A"), 2);
}

TEST_F(KeyedLookupTest, TheDestructorStopsWithKeysStillQueued)
{
    setGate(false);
    {
        KeyedLookup<std::string> lookup({.threadName = THREAD_NAME}, &lookUp);
        for (const std::string key : {"A", "B", "C"})
        {
            (void) lookup.find(key);
        }
        setGate(true); // The lookup in progress finishes; the rest are dropped or done
    }
    EXPECT_LE(lookupsOf("C"), 1);
}

} // namespace
} // namespace Domain
