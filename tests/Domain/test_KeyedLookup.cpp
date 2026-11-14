// Tests for Domain::KeyedLookup (#3718): a lookup made once per key on a worker thread of its own, taken by a
// later find().

#include "Domain/KeyedLookup.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
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
constexpr std::chrono::milliseconds WAIT{5000};

/// What the fake lookup was asked, and where.
struct Calls
{
    std::mutex mutex;
    std::map<std::string, int> byKey;
    std::thread::id thread;
    bool gateOpen = true; ///< Closed: a lookup waits until it opens
    int entered = 0;      ///< Lookups begun, gate or not
    std::condition_variable gateChanged;
};

Calls& calls()
{
    static Calls state;
    return state;
}

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
    ASSERT_TRUE(lookup.waitUntilIdle(WAIT));
    EXPECT_EQ(lookup.find("App.1"), std::optional<std::string>("name of App.1"));
    {
        const std::scoped_lock lock(calls().mutex);
        EXPECT_NE(calls().thread, std::this_thread::get_id());
    }
    EXPECT_GE(drainsBegun().load(), 1);
    // Idle comes as the queue empties; the pass's end hook runs just after, on the worker's thread.
    const auto deadline = std::chrono::steady_clock::now() + WAIT;
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
    ASSERT_TRUE(lookup.waitUntilIdle(WAIT));
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(lookup.find("App.1").has_value());
    }
    EXPECT_EQ(lookupsOf("App.1"), 1);
}

TEST_F(KeyedLookupTest, ALookupThatThrowsAnswersNone)
{
    KeyedLookup<std::string> lookup({.threadName = THREAD_NAME}, &lookUp);
    (void) lookup.find("throws");
    ASSERT_TRUE(lookup.waitUntilIdle(WAIT));
    EXPECT_EQ(lookup.find("throws"), std::optional<std::string>(std::string{}));
    EXPECT_EQ(lookupsOf("throws"), 1) << "the answer is kept, not asked again";
}

TEST_F(KeyedLookupTest, AFullQueueTurnsAKeyAwayAndALaterFindAsksAgain)
{
    setGate(false);
    KeyedLookup<std::string> lookup({.threadName = THREAD_NAME, .maxQueued = 1}, &lookUp);
    (void) lookup.find("A");
    {
        std::unique_lock lock(calls().mutex); // "A" is taken off the queue into its (held) lookup
        ASSERT_TRUE(calls().gateChanged.wait_for(lock, WAIT, [] { return calls().entered == 1; }));
    }
    EXPECT_FALSE(lookup.find("B").has_value()) << "queued: the one place in the queue";
    EXPECT_FALSE(lookup.find("C").has_value()) << "turned away";
    EXPECT_EQ(lookup.size(), 2U) << "A in progress and B waiting; C isn't held";
    setGate(true);
    ASSERT_TRUE(lookup.waitUntilIdle(WAIT));
    EXPECT_FALSE(lookup.find("C").has_value()) << "asked again: queued now";
    ASSERT_TRUE(lookup.waitUntilIdle(WAIT));
    EXPECT_EQ(lookup.find("C"), std::optional<std::string>("name of C"));
    EXPECT_EQ(lookupsOf("C"), 1);
}

TEST_F(KeyedLookupTest, AFullTableForgetsWhatWasAnsweredAndAsksAgain)
{
    KeyedLookup<std::string> lookup({.threadName = THREAD_NAME, .maxEntries = 2}, &lookUp);
    for (const std::string key : {"A", "B"})
    {
        (void) lookup.find(key);
        ASSERT_TRUE(lookup.waitUntilIdle(WAIT));
        EXPECT_TRUE(lookup.find(key).has_value());
    }
    EXPECT_FALSE(lookup.find("C").has_value()) << "the answers taken are forgotten to make room";
    ASSERT_TRUE(lookup.waitUntilIdle(WAIT));
    EXPECT_TRUE(lookup.find("C").has_value());
    EXPECT_LE(lookup.size(), 2U);
    EXPECT_FALSE(lookup.find("A").has_value()) << "forgotten: asked again";
    ASSERT_TRUE(lookup.waitUntilIdle(WAIT));
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
