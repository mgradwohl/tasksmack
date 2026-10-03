/// @file test_ProcessActionsContract.cpp
/// @brief Cross-platform contract tests for Platform::IProcessActions via Platform::makeProcessActions()

#include "Platform/Factory.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on
#else
#include <unistd.h>
#endif

namespace Platform
{
namespace
{

[[nodiscard]] std::int32_t ownPid()
{
#ifdef _WIN32
    return static_cast<std::int32_t>(GetCurrentProcessId());
#else
    return static_cast<std::int32_t>(getpid());
#endif
}

/// This test process as the platform's own probe reports it: the start time the UI would pass.
/// A default ProcessCounters (pid 0) if the probe did not list it.
[[nodiscard]] ProcessCounters ownCountersFromProbe()
{
    const auto probe = makeProcessProbe();
    if (!probe)
    {
        return {};
    }
    const auto counters = probe->enumerate();
    const auto it = std::ranges::find(counters, ownPid(), &ProcessCounters::pid);
    return (it != counters.end()) ? *it : ProcessCounters{};
}

TEST(ProcessActionsContractTest, FactoryConstructs)
{
    const auto actions = makeProcessActions();
    ASSERT_NE(actions, nullptr);
}

TEST(ProcessActionsContractTest, NonExistentPidFailsGracefully)
{
    const auto actions = makeProcessActions();
    ASSERT_NE(actions, nullptr);

    const auto caps = actions->actionCapabilities();
    const ProcessTarget nonExistent{.pid = std::numeric_limits<int32_t>::max(), .startTimeTicks = 1};

    if (caps.canTerminate)
    {
        const auto result = actions->terminate(nonExistent);
        EXPECT_FALSE(result.success);
        EXPECT_GT(result.errorMessage.size(), 0ULL);
    }

    if (caps.canKill)
    {
        const auto result = actions->kill(nonExistent);
        EXPECT_FALSE(result.success);
        EXPECT_GT(result.errorMessage.size(), 0ULL);
    }

    if (caps.canStop)
    {
        const auto result = actions->stop(nonExistent);
        EXPECT_FALSE(result.success);
        EXPECT_GT(result.errorMessage.size(), 0ULL);
    }

    if (caps.canContinue)
    {
        const auto result = actions->resume(nonExistent);
        EXPECT_FALSE(result.success);
        EXPECT_GT(result.errorMessage.size(), 0ULL);
    }

    if (caps.canSetPriority)
    {
        const auto result = actions->setPriority(nonExistent, 0);
        EXPECT_FALSE(result.success);
        EXPECT_GT(result.errorMessage.size(), 0ULL);
    }
}

// =============================================================================
// Identity (#973): an action reaches a process only if it is the one the target names. These act
// on this test process with its current priority, and restore it where that may not round-trip.
// =============================================================================

TEST(ProcessActionsContractTest, ActsWhenTheStartTimeMatchesTheProbes)
{
    // The real probe's start time must be accepted by the real actions. If the two ever read the
    // start time from different sources or in different units, every action from the UI would be
    // refused, and this is the test that would say so.
    const auto actions = makeProcessActions();
    ASSERT_NE(actions, nullptr);
    ASSERT_TRUE(actions->actionCapabilities().canSetPriority);
    const auto own = ownCountersFromProbe();
    ASSERT_NE(own.pid, 0);
    ASSERT_NE(own.startTimeTicks, 0ULL);

#ifdef _WIN32
    // own.nice round-trips to the same class for Normal and the other ordinary classes, but not
    // for Realtime (probe -20, set as High) or before the probe has filled nice in (0, Normal);
    // restore the class exactly so a runner started at either keeps it.
    const DWORD originalClass = GetPriorityClass(GetCurrentProcess());
#endif

    const auto result = actions->setPriority({.pid = own.pid, .startTimeTicks = own.startTimeTicks}, own.nice);

#ifdef _WIN32
    if (originalClass != 0)
    {
        SetPriorityClass(GetCurrentProcess(), originalClass);
    }
#endif
    EXPECT_TRUE(result.success) << result.errorMessage;
}

TEST(ProcessActionsContractTest, RefusesWhenThePidBelongsToADifferentProcess)
{
    // A start time one tick off stands in for a reused PID: same number, different process.
    const auto actions = makeProcessActions();
    ASSERT_NE(actions, nullptr);
    const auto own = ownCountersFromProbe();
    ASSERT_NE(own.pid, 0);
    const ProcessTarget reused{.pid = own.pid, .startTimeTicks = own.startTimeTicks + 1};

    const auto priority = actions->setPriority(reused, own.nice);
    EXPECT_FALSE(priority.success);
    EXPECT_NE(priority.errorMessage.find("different process"), std::string::npos) << priority.errorMessage;

    if (actions->actionCapabilities().canContinue)
    {
        const auto resume = actions->resume(reused);
        EXPECT_FALSE(resume.success);
        EXPECT_NE(resume.errorMessage.find("different process"), std::string::npos) << resume.errorMessage;
    }
}

TEST(ProcessActionsContractTest, RefusesWhenTheStartTimeIsUnknown)
{
    const auto actions = makeProcessActions();
    ASSERT_NE(actions, nullptr);
    const ProcessTarget unknown{.pid = ownPid(), .startTimeTicks = 0};

    const auto priority = actions->setPriority(unknown, 0);
    EXPECT_FALSE(priority.success);
    EXPECT_NE(priority.errorMessage.find("Cannot confirm"), std::string::npos) << priority.errorMessage;

    if (actions->actionCapabilities().canContinue)
    {
        const auto resume = actions->resume(unknown);
        EXPECT_FALSE(resume.success);
        EXPECT_NE(resume.errorMessage.find("Cannot confirm"), std::string::npos) << resume.errorMessage;
    }
}

TEST(ProcessActionsContractTest, ResumeReachesTheProcessWhenTheStartTimeMatches)
{
    // SIGCONT to a running process is a no-op, so this exercises the whole signal path (pidfd,
    // identity check, send) on this process without disturbing it.
    const auto actions = makeProcessActions();
    ASSERT_NE(actions, nullptr);
    if (!actions->actionCapabilities().canContinue)
    {
        GTEST_SKIP() << "Resume is not supported on this platform";
    }
    const auto own = ownCountersFromProbe();
    ASSERT_NE(own.pid, 0);

    const auto result = actions->resume({.pid = own.pid, .startTimeTicks = own.startTimeTicks});

    EXPECT_TRUE(result.success) << result.errorMessage;
}

} // namespace
} // namespace Platform
