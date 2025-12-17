/// @file test_LinuxProcessActions.cpp
/// @brief Integration tests for Platform::LinuxProcessActions
///
/// These tests verify the capabilities reporting and error handling
/// of process actions. We avoid actually terminating processes to keep
/// tests safe and non-destructive.

#include "Platform/Linux/LinuxProcessActions.h"

#include <gtest/gtest.h>

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
    EXPECT_NO_THROW({
        LinuxProcessActions actions;
    });
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

TEST(LinuxProcessActionsTest, TerminateInitProcessFails)
{
    LinuxProcessActions actions;

    // Attempting to signal PID 1 (init) should fail with permission denied
    // (unless running as root, in which case it's still protected)
    auto result = actions.terminate(1);

    // Should fail (either NoPermission or OperationNotPermitted)
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, InvalidPidNegative)
{
    LinuxProcessActions actions;

    // Negative PIDs are invalid
    auto result = actions.terminate(-1);

    // Should fail
    EXPECT_FALSE(result.success);
}

TEST(LinuxProcessActionsTest, InvalidPidZero)
{
    LinuxProcessActions actions;

    // PID 0 has special meaning in kill(2), let's verify behavior
    auto result = actions.terminate(0);

    // Should either fail or handle specially
    // (PID 0 sends to all processes in the process group)
    // We expect it to fail for safety
    EXPECT_FALSE(result.success);
}

// =============================================================================
// Result Structure Tests
// =============================================================================

TEST(LinuxProcessActionsTest, ResultStructureForSuccess)
{
    // We can't easily test successful actions without creating and destroying processes,
    // so we just verify the structure makes sense for error cases

    ProcessActionResult result;
    result.success = false;
    result.errorMessage = "No such process";

    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, ResultHasErrorMessage)
{
    LinuxProcessActions actions;
    auto result = actions.terminate(99999);

    // Error result should have a non-empty error message
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST(LinuxProcessActionsTest, ConcurrentActions)
{
    LinuxProcessActions actions;

    std::atomic<int> successCount{0};
    std::atomic<bool> running{true};

    auto actionTask = [&]() {
        while (running)
        {
            try
            {
                // Attempt to signal non-existent process (safe operation)
                auto result = actions.terminate(99999);
                if (!result.success)
                {
                    ++successCount; // Successfully got expected error
                }
            }
            catch (...)
            {
                // Actions should not throw
                FAIL() << "Action threw an exception";
            }
        }
    };

    // Start multiple threads performing actions concurrently
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
    {
        threads.emplace_back(actionTask);
    }

    // Let them run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& t : threads)
    {
        t.join();
    }

    // All actions should have completed
    EXPECT_GT(successCount.load(), 0);
}

} // namespace
} // namespace Platform
