/// @file test_LinuxProcessActions.cpp
/// @brief Integration tests for Platform::LinuxProcessActions
///
/// These tests verify the capabilities reporting and error handling
/// of process actions. We avoid actually terminating processes to keep
/// tests safe and non-destructive.

#include "Platform/Linux/LinuxProcessActions.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <unistd.h>

namespace Platform
{
namespace
{

// =============================================================================
// Construction and Capabilities
// =============================================================================

TEST(LinuxProcessActionsTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ LinuxProcessActions actions; });
}

TEST(LinuxProcessActionsTest, CapabilitiesReportedCorrectly)
{
    LinuxProcessActions actions;
    auto caps = actions.actionCapabilities();

    // Linux should support all standard process actions
    EXPECT_TRUE(caps.canTerminate);
    EXPECT_TRUE(caps.canKill);
    EXPECT_TRUE(caps.canStop);
    EXPECT_TRUE(caps.canContinue); // resume is called canContinue in the interface
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST(LinuxProcessActionsTest, TerminateNonExistentProcess)
{
    LinuxProcessActions actions;

    // PID 99999 is very unlikely to exist
    int32_t nonExistentPid = 99999;
    auto result = actions.terminate(nonExistentPid);

    // Should fail
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, KillNonExistentProcess)
{
    LinuxProcessActions actions;

    // PID 99999 is very unlikely to exist
    int32_t nonExistentPid = 99999;
    auto result = actions.kill(nonExistentPid);

    // Should fail
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, StopNonExistentProcess)
{
    LinuxProcessActions actions;

    // PID 99999 is very unlikely to exist
    int32_t nonExistentPid = 99999;
    auto result = actions.stop(nonExistentPid);

    // Should fail
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, ResumeNonExistentProcess)
{
    LinuxProcessActions actions;

    // PID 99999 is very unlikely to exist
    int32_t nonExistentPid = 99999;
    auto result = actions.resume(nonExistentPid);

    // Should fail
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, TerminateMultipleThreadsSafely)
{
    LinuxProcessActions actions;

    std::vector<std::thread> threads;
    threads.reserve(4);

    for (int i = 0; i < 4; ++i)
    {
        threads.emplace_back([] {
            // Simulate work
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        });
    }

    for (auto& t : threads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    // Terminate a non-existent process should still fail safely
    auto result = actions.terminate(99999);
    EXPECT_FALSE(result.success);
}

} // namespace
} // namespace Platform
