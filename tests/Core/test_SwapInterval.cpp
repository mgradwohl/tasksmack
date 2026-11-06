/// @file test_SwapInterval.cpp
/// @brief Core::SwapIntervalCache: Window::setVSync() asks the driver only for a change, and asks for
/// adaptive vsync once per context (#3511).

#include "Core/SwapInterval.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{

using Core::SWAP_INTERVAL_ADAPTIVE;
using Core::SWAP_INTERVAL_IMMEDIATE;
using Core::SWAP_INTERVAL_VSYNC;
using Core::SwapIntervalCache;

// Two distinct context identities: only their addresses matter (distinct values, so no linker folds them).
const int CONTEXT_A = 1;
const int CONTEXT_B = 2;

/// A fake SDL_GL_SetSwapInterval(): records each interval asked for, and refuses adaptive vsync when
/// told to, as Intel's Windows OpenGL driver does ("Negative swap interval unsupported in this GL").
struct FakeDriver
{
    bool adaptiveSupported = true;
    bool refuseAll = false;
    std::vector<int> calls;

    bool operator()(int interval)
    {
        calls.push_back(interval);
        if (refuseAll)
        {
            return false;
        }
        return interval != SWAP_INTERVAL_ADAPTIVE || adaptiveSupported;
    }
};

TEST(SwapIntervalCacheTest, FirstEnablePrefersAdaptiveVsync)
{
    SwapIntervalCache cache;
    FakeDriver driver;
    const auto outcome = cache.apply(true, &CONTEXT_A, [&](int interval) { return driver(interval); });
    EXPECT_EQ(driver.calls, (std::vector{SWAP_INTERVAL_ADAPTIVE}));
    EXPECT_EQ(outcome.calls, 1U);
    EXPECT_TRUE(outcome.accepted);
    EXPECT_FALSE(outcome.adaptiveRefused);
    EXPECT_EQ(outcome.applied, SWAP_INTERVAL_ADAPTIVE);
}

TEST(SwapIntervalCacheTest, RefusedAdaptiveFallsBackToVsyncAndIsNotAskedAgain)
{
    SwapIntervalCache cache;
    FakeDriver driver;
    driver.adaptiveSupported = false;
    const auto set = [&](int interval)
    {
        return driver(interval);
    };

    const auto first = cache.apply(true, &CONTEXT_A, set);
    EXPECT_EQ(driver.calls, (std::vector{SWAP_INTERVAL_ADAPTIVE, SWAP_INTERVAL_VSYNC}));
    EXPECT_TRUE(first.accepted);
    EXPECT_TRUE(first.adaptiveRefused) << "the first refusal is reported, so it is logged once";
    EXPECT_EQ(first.applied, SWAP_INTERVAL_VSYNC);
    EXPECT_TRUE(cache.adaptiveUnsupported());

    // Each resize interaction turns vsync off at its start and on at its end (Application::run()).
    driver.calls.clear();
    for (int interaction = 0; interaction < 64; ++interaction)
    {
        const auto off = cache.apply(false, &CONTEXT_A, set);
        const auto on = cache.apply(true, &CONTEXT_A, set);
        EXPECT_FALSE(off.adaptiveRefused);
        EXPECT_FALSE(on.adaptiveRefused) << "refused once, reported once";
    }
    std::vector<int> expected;
    for (int interaction = 0; interaction < 64; ++interaction)
    {
        expected.push_back(SWAP_INTERVAL_IMMEDIATE);
        expected.push_back(SWAP_INTERVAL_VSYNC);
    }
    EXPECT_EQ(driver.calls, expected) << "two calls an interaction, not three: no adaptive attempt that always fails";
}

TEST(SwapIntervalCacheTest, UnchangedRequestsMakeNoCall)
{
    SwapIntervalCache cache;
    FakeDriver driver;
    const auto set = [&](int interval)
    {
        return driver(interval);
    };
    static_cast<void>(cache.apply(false, &CONTEXT_A, set));
    const auto again = cache.apply(false, &CONTEXT_A, set);
    EXPECT_EQ(again.calls, 0U);
    EXPECT_TRUE(again.accepted);
    static_cast<void>(cache.apply(true, &CONTEXT_A, set));
    const auto onAgain = cache.apply(true, &CONTEXT_A, set);
    EXPECT_EQ(onAgain.calls, 0U);
    EXPECT_EQ(driver.calls, (std::vector{SWAP_INTERVAL_IMMEDIATE, SWAP_INTERVAL_ADAPTIVE}));
}

TEST(SwapIntervalCacheTest, PlanListsOnlyWhatIsNeeded)
{
    SwapIntervalCache cache;
    EXPECT_EQ(cache.plan(true, &CONTEXT_A).count, 2U) << "adaptive, then regular vsync";
    cache.record(SWAP_INTERVAL_ADAPTIVE, false);
    const auto afterRefusal = cache.plan(true, &CONTEXT_A);
    ASSERT_EQ(afterRefusal.count, 1U);
    EXPECT_EQ(afterRefusal.view()[0], SWAP_INTERVAL_VSYNC);
    cache.record(SWAP_INTERVAL_VSYNC, true);
    EXPECT_EQ(cache.plan(true, &CONTEXT_A).count, 0U);
    const auto off = cache.plan(false, &CONTEXT_A);
    ASSERT_EQ(off.count, 1U);
    EXPECT_EQ(off.view()[0], SWAP_INTERVAL_IMMEDIATE);
}

TEST(SwapIntervalCacheTest, ANewContextStartsOver)
{
    // A context replaced after a GPU reset (#3484) has its own swap interval: set it again, and ask
    // for adaptive vsync again.
    SwapIntervalCache cache;
    FakeDriver driver;
    driver.adaptiveSupported = false;
    const auto set = [&](int interval)
    {
        return driver(interval);
    };
    static_cast<void>(cache.apply(true, &CONTEXT_A, set));
    driver.calls.clear();
    const auto replaced = cache.apply(true, &CONTEXT_B, set);
    EXPECT_EQ(driver.calls, (std::vector{SWAP_INTERVAL_ADAPTIVE, SWAP_INTERVAL_VSYNC}));
    EXPECT_TRUE(replaced.adaptiveRefused) << "a new context's refusal is its own";
    EXPECT_EQ(replaced.applied, SWAP_INTERVAL_VSYNC);
}

TEST(SwapIntervalCacheTest, ARefusedIntervalIsAskedForAgainNextTime)
{
    SwapIntervalCache cache;
    FakeDriver driver;
    driver.refuseAll = true;
    const auto set = [&](int interval)
    {
        return driver(interval);
    };
    const auto off = cache.apply(false, &CONTEXT_A, set);
    EXPECT_FALSE(off.accepted);
    EXPECT_FALSE(off.applied.has_value());
    driver.refuseAll = false;
    const auto retry = cache.apply(false, &CONTEXT_A, set);
    EXPECT_EQ(retry.calls, 1U);
    EXPECT_TRUE(retry.accepted);
    EXPECT_EQ(driver.calls, (std::vector{SWAP_INTERVAL_IMMEDIATE, SWAP_INTERVAL_IMMEDIATE}));
}

TEST(SwapIntervalCacheTest, ANewContextWithAReusedHandleIsSetAgain)
{
    // A destroyed context's handle can come back for the next one (#3511 review). Window invalidates the
    // cache when it makes or destroys a context, so the new one gets its interval, not the old one's.
    SwapIntervalCache cache;
    FakeDriver driver;
    driver.adaptiveSupported = false;
    const auto set = [&](int interval)
    {
        return driver(interval);
    };
    static_cast<void>(cache.apply(true, &CONTEXT_A, set));
    cache.invalidate(); // ~Window(): the context is destroyed
    cache.invalidate(); // Window(): a new one is made, with the same handle
    driver.calls.clear();
    const auto reused = cache.apply(true, &CONTEXT_A, set);
    EXPECT_EQ(driver.calls, (std::vector{SWAP_INTERVAL_ADAPTIVE, SWAP_INTERVAL_VSYNC})) << "set again, adaptive asked again";
    EXPECT_TRUE(reused.adaptiveRefused) << "the new context's refusal is its own, and logged";
    EXPECT_EQ(reused.applied, SWAP_INTERVAL_VSYNC);

    // From then on the new context is remembered as before.
    driver.calls.clear();
    const auto again = cache.apply(true, &CONTEXT_A, set);
    EXPECT_EQ(again.calls, 0U);
    EXPECT_FALSE(again.adaptiveRefused);
}

TEST(SwapIntervalCacheTest, InvalidateBeforeAnyRequestChangesNothing)
{
    SwapIntervalCache cache;
    FakeDriver driver;
    cache.invalidate();
    const auto off = cache.apply(false, &CONTEXT_A, [&](int interval) { return driver(interval); });
    EXPECT_EQ(driver.calls, (std::vector{SWAP_INTERVAL_IMMEDIATE}));
    EXPECT_TRUE(off.accepted);
}

TEST(SwapIntervalCacheTest, VsyncRefusedEntirelyLeavesTheIntervalUnknown)
{
    SwapIntervalCache cache;
    FakeDriver driver;
    driver.refuseAll = true;
    const auto on = cache.apply(true, &CONTEXT_A, [&](int interval) { return driver(interval); });
    EXPECT_FALSE(on.accepted);
    EXPECT_EQ(on.calls, 2U);
    EXPECT_FALSE(cache.applied().has_value());
}

} // namespace
