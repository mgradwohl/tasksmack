/// @file test_ProcessActionHelpers.cpp
/// @brief Mock-IProcessActions integration tests for App::Detail::dispatchProcessAction() and
/// formatActionResultMessage(), extracted from ProcessDetailsPanel::dispatchConfirmedAction().
///
/// This is the "integration tests for process actions (with mock IProcessActions)" item from
/// #415. ProcessDetailsPanel.cpp itself is not linked into TaskSmackTests - it pulls in real
/// ImGui/ImPlot calls that aren't satisfiable in this test binary, the same reason
/// test_ProcessesPanel.cpp doesn't link ProcessesPanel.cpp either - so these tests exercise the
/// actual dispatch decision logic directly against a mock, which is the part #415 cared about.

#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace App::Detail
{
namespace
{

TEST(ProcessActionHelpersTest, TerminateCallsTerminateWithPid)
{
    TestMocks::MockProcessActions mock;
    const auto result = dispatchProcessAction(mock, ProcessAction::Terminate, 4242);

    EXPECT_EQ(mock.terminateCount(), 1);
    EXPECT_EQ(mock.lastTerminatePid(), 4242);
    EXPECT_EQ(mock.killCount(), 0);
    EXPECT_EQ(mock.stopCount(), 0);
    EXPECT_EQ(mock.resumeCount(), 0);
    EXPECT_TRUE(result.success);
}

TEST(ProcessActionHelpersTest, KillCallsKillWithPid)
{
    TestMocks::MockProcessActions mock;
    const auto result = dispatchProcessAction(mock, ProcessAction::Kill, 777);

    EXPECT_EQ(mock.killCount(), 1);
    EXPECT_EQ(mock.lastKillPid(), 777);
    EXPECT_EQ(mock.terminateCount(), 0);
    EXPECT_TRUE(result.success);
}

TEST(ProcessActionHelpersTest, StopCallsStopWithPid)
{
    TestMocks::MockProcessActions mock;
    const auto result = dispatchProcessAction(mock, ProcessAction::Stop, 88);

    EXPECT_EQ(mock.stopCount(), 1);
    EXPECT_EQ(mock.lastStopPid(), 88);
    EXPECT_TRUE(result.success);
}

TEST(ProcessActionHelpersTest, ResumeCallsResumeWithPid)
{
    TestMocks::MockProcessActions mock;
    const auto result = dispatchProcessAction(mock, ProcessAction::Resume, 99);

    EXPECT_EQ(mock.resumeCount(), 1);
    EXPECT_EQ(mock.lastResumePid(), 99);
    EXPECT_TRUE(result.success);
}

TEST(ProcessActionHelpersTest, NoneCallsNothingAndReturnsError)
{
    TestMocks::MockProcessActions mock;
    const auto result = dispatchProcessAction(mock, ProcessAction::None, 1);

    EXPECT_EQ(mock.terminateCount(), 0);
    EXPECT_EQ(mock.killCount(), 0);
    EXPECT_EQ(mock.stopCount(), 0);
    EXPECT_EQ(mock.resumeCount(), 0);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "No action selected");
}

TEST(ProcessActionHelpersTest, PropagatesMockFailureResult)
{
    TestMocks::MockProcessActions mock;
    mock.setKillResult(Platform::ProcessActionResult::error("Access is denied."));

    const auto result = dispatchProcessAction(mock, ProcessAction::Kill, 5);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Access is denied.");
}

TEST(ProcessActionHelpersTest, ActionVerbsAreLowercase)
{
    EXPECT_STREQ(actionVerb(ProcessAction::Terminate), "terminate");
    EXPECT_STREQ(actionVerb(ProcessAction::Kill), "kill");
    EXPECT_STREQ(actionVerb(ProcessAction::Stop), "stop");
    EXPECT_STREQ(actionVerb(ProcessAction::Resume), "resume");
    EXPECT_STREQ(actionVerb(ProcessAction::None), "");
}

TEST(ProcessActionHelpersTest, FormatSuccessMessage)
{
    const auto result = Platform::ProcessActionResult::ok();
    EXPECT_EQ(formatActionResultMessage(ProcessAction::Terminate, 123, result), "Success: terminate sent to PID 123");
    EXPECT_EQ(formatActionResultMessage(ProcessAction::Kill, 456, result), "Success: kill sent to PID 456");
}

TEST(ProcessActionHelpersTest, FormatErrorMessage)
{
    const auto result = Platform::ProcessActionResult::error("Process not found");
    EXPECT_EQ(formatActionResultMessage(ProcessAction::Kill, 789, result), "Error: Process not found");
}

TEST(ProcessActionHelpersTest, DispatchThenFormatEndToEnd)
{
    // Exercises the exact two-call sequence ProcessDetailsPanel::dispatchConfirmedAction() runs,
    // end to end against a mock, without needing to link ProcessDetailsPanel.cpp itself.
    TestMocks::MockProcessActions mock;
    mock.setStopResult(Platform::ProcessActionResult::error("Operation not permitted"));

    const auto result = dispatchProcessAction(mock, ProcessAction::Stop, 321);
    const auto message = formatActionResultMessage(ProcessAction::Stop, 321, result);

    EXPECT_EQ(mock.lastStopPid(), 321);
    EXPECT_EQ(message, "Error: Operation not permitted");
}

} // namespace
} // namespace App::Detail
