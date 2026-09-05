/// @file test_WindowsProcessActions.cpp
/// @brief Integration tests for Platform::WindowsProcessActions
///
/// These tests verify the capabilities reporting and error handling
/// of process actions. We avoid actually terminating processes to keep
/// tests safe and non-destructive.

#include "Platform/Windows/WindowsProcessActions.h"
#include "Platform/Windows/WindowsProcessActionsMath.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <cstdint>
#include <limits>

namespace Platform
{

// =============================================================================
// niceToPriorityClass: pure mapping from Unix nice value to Windows priority class,
// no process required. Covers all five threshold branches plus their boundaries.
// =============================================================================

TEST(NiceToPriorityClassTest, BelowHighThresholdMapsToHigh)
{
    EXPECT_EQ(niceToPriorityClass(-20), static_cast<uint32_t>(HIGH_PRIORITY_CLASS));
    EXPECT_EQ(niceToPriorityClass(-11), static_cast<uint32_t>(HIGH_PRIORITY_CLASS));
}

TEST(NiceToPriorityClassTest, AtHighThresholdMapsToAboveNormal)
{
    // HIGH_THRESHOLD = -10 is the first value no longer < HIGH_THRESHOLD.
    EXPECT_EQ(niceToPriorityClass(-10), static_cast<uint32_t>(ABOVE_NORMAL_PRIORITY_CLASS));
    EXPECT_EQ(niceToPriorityClass(-6), static_cast<uint32_t>(ABOVE_NORMAL_PRIORITY_CLASS));
}

TEST(NiceToPriorityClassTest, AtAboveNormalThresholdMapsToNormal)
{
    // ABOVE_NORMAL_THRESHOLD = -5.
    EXPECT_EQ(niceToPriorityClass(-5), static_cast<uint32_t>(NORMAL_PRIORITY_CLASS));
    EXPECT_EQ(niceToPriorityClass(0), static_cast<uint32_t>(NORMAL_PRIORITY_CLASS));
    EXPECT_EQ(niceToPriorityClass(4), static_cast<uint32_t>(NORMAL_PRIORITY_CLASS));
}

TEST(NiceToPriorityClassTest, AtBelowNormalThresholdMapsToBelowNormal)
{
    // BELOW_NORMAL_THRESHOLD = 5.
    EXPECT_EQ(niceToPriorityClass(5), static_cast<uint32_t>(BELOW_NORMAL_PRIORITY_CLASS));
    EXPECT_EQ(niceToPriorityClass(14), static_cast<uint32_t>(BELOW_NORMAL_PRIORITY_CLASS));
}

TEST(NiceToPriorityClassTest, AtIdleThresholdAndAboveMapsToIdle)
{
    // IDLE_THRESHOLD = 15.
    EXPECT_EQ(niceToPriorityClass(15), static_cast<uint32_t>(IDLE_PRIORITY_CLASS));
    EXPECT_EQ(niceToPriorityClass(19), static_cast<uint32_t>(IDLE_PRIORITY_CLASS));
}

TEST(WindowsProcessActionsTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ WindowsProcessActions actions; });
}

TEST(WindowsProcessActionsTest, CapabilitiesReportedCorrectly)
{
    WindowsProcessActions actions;
    const auto caps = actions.actionCapabilities();

    EXPECT_TRUE(caps.canTerminate);
    EXPECT_TRUE(caps.canKill);
    EXPECT_FALSE(caps.canStop);
    EXPECT_FALSE(caps.canContinue);
}

TEST(WindowsProcessActionsTest, StopNotSupported)
{
    WindowsProcessActions actions;

    const auto result = actions.stop(1);
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(WindowsProcessActionsTest, ResumeNotSupported)
{
    WindowsProcessActions actions;

    const auto result = actions.resume(1);
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(WindowsProcessActionsTest, SetPriorityRejectsInvalidPid)
{
    WindowsProcessActions actions;

    // Test PID 0 (boundary)
    auto result = actions.setPriority(0, 0);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Invalid PID");

    // Test negative PID (covers full range of `pid <= 0` check)
    result = actions.setPriority(-1, 0);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Invalid PID");
}

TEST(WindowsProcessActionsTest, TerminateNonExistentProcess)
{
    WindowsProcessActions actions;

    const int32_t nonExistentPid = std::numeric_limits<int32_t>::max();
    const auto result = actions.terminate(nonExistentPid);

    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(WindowsProcessActionsTest, KillNonExistentProcess)
{
    WindowsProcessActions actions;

    const int32_t nonExistentPid = std::numeric_limits<int32_t>::max();
    const auto result = actions.kill(nonExistentPid);

    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

} // namespace Platform
