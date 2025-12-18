/// @file test_WindowsProcessActions.cpp
/// @brief Integration tests for Platform::WindowsProcessActions
///
/// These tests verify the capabilities reporting and error handling
/// of process actions. We avoid actually terminating processes to keep
/// tests safe and non-destructive.

#include "Platform/Windows/WindowsProcessActions.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace Platform
{
namespace
{
} // namespace

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
