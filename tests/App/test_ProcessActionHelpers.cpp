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
#include "Domain/ProcessSnapshot.h"
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
    const auto result = dispatchProcessAction(mock, ProcessAction::Terminate, {.pid = 4242, .startTimeTicks = 1000});

    EXPECT_EQ(mock.terminateCount(), 1);
    EXPECT_EQ(mock.lastTerminatePid(), 4242);
    EXPECT_EQ(mock.killCount(), 0);
    EXPECT_EQ(mock.stopCount(), 0);
    EXPECT_EQ(mock.resumeCount(), 0);
    EXPECT_TRUE(result.success);
}

TEST(ProcessActionHelpersTest, DispatchPassesTheStartTimeThrough)
{
    // The start time is what lets the platform tell the selected process from a later one given
    // the same PID (#973); dropping it on the way would quietly turn every action back into PID-only.
    TestMocks::MockProcessActions mock;
    const auto result = dispatchProcessAction(mock, ProcessAction::Kill, {.pid = 4242, .startTimeTicks = 133'000'000'000ULL});

    EXPECT_TRUE(result.success);
    EXPECT_EQ(mock.lastTarget().pid, 4242);
    EXPECT_EQ(mock.lastTarget().startTimeTicks, 133'000'000'000ULL);
}

TEST(ProcessActionHelpersTest, TargetForSelectionTakesTheConfirmedSnapshotsStartTime)
{
    Domain::ProcessSnapshot snapshot;
    snapshot.pid = 4242;
    snapshot.startTimeTicks = 987'654'321ULL;

    const Platform::ProcessTarget target = targetForSelection(4242, &snapshot);

    EXPECT_EQ(target.pid, 4242);
    EXPECT_EQ(target.startTimeTicks, 987'654'321ULL);
}

TEST(ProcessActionHelpersTest, TargetForSelectionWithoutASnapshotLeavesTheStartTimeUnknown)
{
    // No snapshot has confirmed the selection yet, so there is nothing to vouch for the process
    // behind the PID. Unknown makes the platform refuse rather than act on the PID alone.
    const Platform::ProcessTarget target = targetForSelection(4242, nullptr);

    EXPECT_EQ(target.pid, 4242);
    EXPECT_EQ(target.startTimeTicks, 0ULL);
}

TEST(ProcessActionHelpersTest, KillCallsKillWithPid)
{
    TestMocks::MockProcessActions mock;
    const auto result = dispatchProcessAction(mock, ProcessAction::Kill, {.pid = 777, .startTimeTicks = 1000});

    EXPECT_EQ(mock.killCount(), 1);
    EXPECT_EQ(mock.lastKillPid(), 777);
    EXPECT_EQ(mock.terminateCount(), 0);
    EXPECT_TRUE(result.success);
}

TEST(ProcessActionHelpersTest, StopCallsStopWithPid)
{
    TestMocks::MockProcessActions mock;
    const auto result = dispatchProcessAction(mock, ProcessAction::Stop, {.pid = 88, .startTimeTicks = 1000});

    EXPECT_EQ(mock.stopCount(), 1);
    EXPECT_EQ(mock.lastStopPid(), 88);
    EXPECT_TRUE(result.success);
}

TEST(ProcessActionHelpersTest, ResumeCallsResumeWithPid)
{
    TestMocks::MockProcessActions mock;
    const auto result = dispatchProcessAction(mock, ProcessAction::Resume, {.pid = 99, .startTimeTicks = 1000});

    EXPECT_EQ(mock.resumeCount(), 1);
    EXPECT_EQ(mock.lastResumePid(), 99);
    EXPECT_TRUE(result.success);
}

TEST(ProcessActionHelpersTest, NoneCallsNothingAndReturnsError)
{
    TestMocks::MockProcessActions mock;
    const auto result = dispatchProcessAction(mock, ProcessAction::None, {.pid = 1, .startTimeTicks = 1000});

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

    const auto result = dispatchProcessAction(mock, ProcessAction::Kill, {.pid = 5, .startTimeTicks = 1000});

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Access is denied.");
}

TEST(ProcessActionHelpersTest, ActionVerbsAreLowercase)
{
    EXPECT_STREQ(actionVerb(ProcessAction::Terminate), "terminate");
    EXPECT_STREQ(actionVerb(ProcessAction::Kill), "kill");
    EXPECT_STREQ(actionVerb(ProcessAction::Stop), "suspend"); // The Suspend button's word, not "stop" (#1203)
    EXPECT_STREQ(actionVerb(ProcessAction::Resume), "resume");
    EXPECT_STREQ(actionVerb(ProcessAction::None), "");
}

TEST(ProcessActionHelpersTest, ActionLabelsMatchTheButtons)
{
    EXPECT_STREQ(actionLabel(ProcessAction::Terminate), "Terminate");
    EXPECT_STREQ(actionLabel(ProcessAction::Kill), "Kill");
    EXPECT_STREQ(actionLabel(ProcessAction::Stop), "Suspend");
    EXPECT_STREQ(actionLabel(ProcessAction::Resume), "Resume");
    EXPECT_STREQ(actionLabel(ProcessAction::None), "");
}

TEST(ProcessActionHelpersTest, ConfirmTitleNamesTheActionAndTheProcess)
{
    EXPECT_EQ(confirmTitle(ProcessAction::Kill, "firefox", 1234), "Kill firefox (PID 1234)?");
    EXPECT_EQ(confirmTitle(ProcessAction::Stop, "make", 7), "Suspend make (PID 7)?");
}

TEST(ProcessActionHelpersTest, ConfirmBodyStatesTheOutcome)
{
    EXPECT_EQ(confirmBody(ProcessAction::Kill, "firefox", 1234), "firefox (PID 1234) will end immediately, without saving its work.");
    for (const auto action : {ProcessAction::Terminate, ProcessAction::Kill, ProcessAction::Stop, ProcessAction::Resume})
    {
        const auto body = confirmBody(action, "proc", 42);
        EXPECT_TRUE(body.contains("proc (PID 42)")) << body;
        EXPECT_FALSE(body.contains("Are you sure")) << body;
    }
}

// The outcome is a flag, not a word to search for: neither a success whose text happens to
// contain "Error"/"Failed" nor a failure without those words may be coloured wrongly (#1203).
TEST(ProcessActionHelpersTest, FormatSuccessMessage)
{
    const auto result = Platform::ProcessActionResult::ok();
    const auto terminate = formatActionResultMessage(ProcessAction::Terminate, 123, result);
    EXPECT_TRUE(terminate.ok);
    EXPECT_EQ(terminate.text, "Terminate sent to PID 123");
    const auto suspend = formatActionResultMessage(ProcessAction::Stop, 456, result);
    EXPECT_TRUE(suspend.ok);
    EXPECT_EQ(suspend.text, "Suspend sent to PID 456");
}

TEST(ProcessActionHelpersTest, FormatErrorMessage)
{
    const auto result = Platform::ProcessActionResult::error("Process not found");
    const auto message = formatActionResultMessage(ProcessAction::Kill, 789, result);
    EXPECT_FALSE(message.ok);
    EXPECT_EQ(message.text, "Could not kill PID 789: Process not found");
}

TEST(ProcessActionHelpersTest, FailureIsFlaggedWhateverItsWording)
{
    const auto message = formatActionResultMessage(ProcessAction::Kill, 5, Platform::ProcessActionResult::error("Access is denied."));
    EXPECT_FALSE(message.ok);
    EXPECT_FALSE(message.text.contains("Error"));
    EXPECT_FALSE(message.empty());

    const ActionResultMessage none{};
    EXPECT_TRUE(none.empty());
}

TEST(ProcessActionHelpersTest, DispatchThenFormatEndToEnd)
{
    // Exercises the exact two-call sequence ProcessDetailsPanel::dispatchConfirmedAction() runs,
    // end to end against a mock, without needing to link ProcessDetailsPanel.cpp itself.
    TestMocks::MockProcessActions mock;
    mock.setStopResult(Platform::ProcessActionResult::error("Operation not permitted"));

    const auto result = dispatchProcessAction(mock, ProcessAction::Stop, {.pid = 321, .startTimeTicks = 1000});
    const auto message = formatActionResultMessage(ProcessAction::Stop, 321, result);

    EXPECT_EQ(mock.lastStopPid(), 321);
    EXPECT_FALSE(message.ok);
    EXPECT_EQ(message.text, "Could not suspend PID 321: Operation not permitted");
}

// Only the actions that end the process get the danger style (#1273).
TEST(ProcessActionHelpersTest, OnlyTerminateAndKillAreDestructive)
{
    EXPECT_TRUE(isDestructiveAction(ProcessAction::Terminate));
    EXPECT_TRUE(isDestructiveAction(ProcessAction::Kill));
    EXPECT_FALSE(isDestructiveAction(ProcessAction::Stop));
    EXPECT_FALSE(isDestructiveAction(ProcessAction::Resume));
    EXPECT_FALSE(isDestructiveAction(ProcessAction::None));
}

} // namespace
} // namespace App::Detail
