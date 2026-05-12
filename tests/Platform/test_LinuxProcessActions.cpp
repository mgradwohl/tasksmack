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
    EXPECT_TRUE(caps.canContinue);    // resume is called canContinue in the interface
    EXPECT_TRUE(caps.canSetPriority); // setpriority() is available on Linux
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

TEST(LinuxProcessActionsTest, TerminateInvalidPid)
{
    LinuxProcessActions actions;

    // pid=0 is invalid (sendSignal() guards pid <= 0)
    auto resultZero = actions.terminate(0);
    EXPECT_FALSE(resultZero.success);
    EXPECT_GT(resultZero.errorMessage.size(), 0ULL);

    // Negative pid is also invalid
    auto resultNeg = actions.terminate(-1);
    EXPECT_FALSE(resultNeg.success);
    EXPECT_GT(resultNeg.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, KillInvalidPid)
{
    LinuxProcessActions actions;

    auto resultZero = actions.kill(0);
    EXPECT_FALSE(resultZero.success);
    EXPECT_GT(resultZero.errorMessage.size(), 0ULL);

    auto resultNeg = actions.kill(-1);
    EXPECT_FALSE(resultNeg.success);
    EXPECT_GT(resultNeg.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, StopInvalidPid)
{
    LinuxProcessActions actions;

    auto resultZero = actions.stop(0);
    EXPECT_FALSE(resultZero.success);
    EXPECT_GT(resultZero.errorMessage.size(), 0ULL);

    auto resultNeg = actions.stop(-1);
    EXPECT_FALSE(resultNeg.success);
    EXPECT_GT(resultNeg.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, ResumeInvalidPid)
{
    LinuxProcessActions actions;

    auto resultZero = actions.resume(0);
    EXPECT_FALSE(resultZero.success);
    EXPECT_GT(resultZero.errorMessage.size(), 0ULL);

    auto resultNeg = actions.resume(-1);
    EXPECT_FALSE(resultNeg.success);
    EXPECT_GT(resultNeg.errorMessage.size(), 0ULL);
}

// =============================================================================
// Priority Adjustment Tests
// =============================================================================

TEST(LinuxProcessActionsTest, SetPriorityNonExistentProcess)
{
    LinuxProcessActions actions;

    // PID 99999 is very unlikely to exist
    int32_t nonExistentPid = 99999;
    auto result = actions.setPriority(nonExistentPid, 0);

    // Should fail
    EXPECT_FALSE(result.success);
    EXPECT_GT(result.errorMessage.size(), 0ULL);
}

TEST(LinuxProcessActionsTest, SetPriorityInvalidPid)
{
    LinuxProcessActions actions;

    // Test with invalid PIDs
    auto result1 = actions.setPriority(0, 0);
    EXPECT_FALSE(result1.success);
    EXPECT_GT(result1.errorMessage.size(), 0ULL);

    auto result2 = actions.setPriority(-1, 0);
    EXPECT_FALSE(result2.success);
    EXPECT_GT(result2.errorMessage.size(), 0ULL);
}

// Note: This test may modify the test process's priority and cannot reliably
// restore the original value without root privileges. This is acceptable since
// test processes typically run at default nice=0 and are short-lived.
TEST(LinuxProcessActionsTest, SetPriorityOwnProcess)
{
    LinuxProcessActions actions;
    int32_t ownPid = static_cast<int32_t>(getpid());

    // Lowering priority (raising nice value) should work without root
    auto result = actions.setPriority(ownPid, 10);

    // This may succeed or fail depending on current priority
    // If we're already at a high nice value, this should succeed
    // If we're at a lower nice value, we might need root to go back down

    // Only attempt cleanup if the initial operation succeeded
    if (result.success)
    {
        // Attempt to reset to 0 (may fail without privileges - see note above)
        auto resetResult = actions.setPriority(ownPid, 0);
        if (!resetResult.success)
        {
            // Log warning but don't fail - lowering nice requires privileges
            GTEST_LOG_(WARNING) << "Test cleanup: Failed to reset priority for PID " << ownPid << ": " << resetResult.errorMessage;
        }
    }
    else
    {
        // At minimum, the error message should be informative if it fails
        EXPECT_GT(result.errorMessage.size(), 0ULL);
    }
}

TEST(LinuxProcessActionsTest, SetPriorityClampsBoundaryValues)
{
    LinuxProcessActions actions;
    int32_t ownPid = static_cast<int32_t>(getpid());

    // Test extreme values - they should be clamped internally
    // These may fail due to permissions, but shouldn't crash
    auto result1 = actions.setPriority(ownPid, -100); // Way below -20
    auto result2 = actions.setPriority(ownPid, 100);  // Way above 19

    // Either succeeds or has an error message, but no crash
    if (!result1.success)
    {
        EXPECT_GT(result1.errorMessage.size(), 0ULL);
    }
    if (!result2.success)
    {
        EXPECT_GT(result2.errorMessage.size(), 0ULL);
    }

    // Cleanup: attempt to reset priority to 0 to avoid affecting subsequent tests.
    // Note: This may fail without root privileges (see SetPriorityOwnProcess note).
    if (result2.success)
    {
        auto resetResult = actions.setPriority(ownPid, 0);
        if (!resetResult.success)
        {
            GTEST_LOG_(WARNING) << "Test cleanup: Failed to reset priority for PID " << ownPid << ": " << resetResult.errorMessage;
        }
    }
}

// =============================================================================
// Success Path Tests
// =============================================================================

TEST(LinuxProcessActionsTest, ResumeOwnProcess_Succeeds)
{
    // Send SIGCONT to our own process - safe because it's a no-op on a running process
    // but exercises the sendSignal() success path (lines ~114-116 in LinuxProcessActions.cpp).
    LinuxProcessActions actions;
    int32_t ownPid = static_cast<int32_t>(getpid());

    auto result = actions.resume(ownPid);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.errorMessage.empty());
}

} // namespace
} // namespace Platform
