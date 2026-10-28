/// @file test_ProcessActionHelpers.cpp
/// @brief Mock-IProcessActions integration tests for App::Detail::dispatchProcessAction() and
/// formatActionResultMessage(), which Process Details' and the Processes table's actions run.
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
#include <format>
#include <string>
#include <tuple>

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

// #3063: each Signal* action sends its own signal, says it plainly, and retries as SendSignal.
TEST(ProcessActionHelpersTest, EachOtherSignalSendsItsSignalAndIsWordedForIt)
{
    TestMocks::MockProcessActions mock;
    const Platform::ProcessTarget target{.pid = 321, .startTimeTicks = 1000};
    for (const auto& [action, signal, name] : {std::tuple{ProcessAction::SignalHangup, Platform::ProcessSignal::Hangup, "SIGHUP"},
                                               std::tuple{ProcessAction::SignalInterrupt, Platform::ProcessSignal::Interrupt, "SIGINT"},
                                               std::tuple{ProcessAction::SignalQuit, Platform::ProcessSignal::Quit, "SIGQUIT"},
                                               std::tuple{ProcessAction::SignalUser1, Platform::ProcessSignal::User1, "SIGUSR1"},
                                               std::tuple{ProcessAction::SignalUser2, Platform::ProcessSignal::User2, "SIGUSR2"}})
    {
        EXPECT_TRUE(dispatchProcessAction(mock, action, target).success) << name;
        EXPECT_EQ(mock.lastSignal(), signal) << name;
        EXPECT_STREQ(actionLabel(action), name);
        EXPECT_EQ(confirmTitle(action, "nginx", 321), std::format("Send {} to nginx (PID 321)?", name));
        EXPECT_FALSE(confirmBody(action, "nginx", 321).empty()) << name;
        const auto authorized = authorizedActionFor(action, target);
        ASSERT_TRUE(authorized.has_value()) << name;
        EXPECT_EQ(authorized->kind, Platform::AuthorizedAction::Kind::SendSignal);
        EXPECT_EQ(authorized->signal, signal);
        EXPECT_FALSE(isDestructiveAction(action)) << name;
    }
    EXPECT_EQ(mock.sendSignalCount(), 5);
    EXPECT_EQ(formatActionResultMessage(ProcessAction::SignalHangup, 321, Platform::ProcessActionResult::ok()).text,
              "SIGHUP sent to PID 321");
    EXPECT_EQ(formatActionResultMessage(ProcessAction::SignalQuit, 321, Platform::ProcessActionResult::error("No such process")).text,
              "Could not send SIGQUIT to PID 321: No such process");
    EXPECT_FALSE(signalFor(ProcessAction::Kill).has_value());
}

// #3071: Efficiency mode on and off reach setEfficiencyMode(), are worded for it, and retry as
// SetEfficiencyMode; neither is destructive, and only a Windows Status of "Efficiency Mode" reads as on.
TEST(ProcessActionHelpersTest, EfficiencyModeOnAndOffSetItAndAreWordedForIt)
{
    TestMocks::MockProcessActions mock;
    const Platform::ProcessTarget target{.pid = 321, .startTimeTicks = 1000};
    for (const auto& [action, on, word] :
         {std::tuple{ProcessAction::EfficiencyModeOn, true, "on"}, std::tuple{ProcessAction::EfficiencyModeOff, false, "off"}})
    {
        EXPECT_TRUE(isEfficiencyModeAction(action));
        EXPECT_TRUE(dispatchProcessAction(mock, action, target).success) << word;
        EXPECT_EQ(mock.lastEfficiencyMode(), on);
        EXPECT_EQ(mock.lastTarget(), target);
        EXPECT_EQ(confirmTitle(action, "chrome.exe", 321), std::format("Turn {} Efficiency mode for chrome.exe (PID 321)?", word));
        EXPECT_NE(confirmBody(action, "chrome.exe", 321).find("chrome.exe (PID 321)"), std::string::npos) << word;
        EXPECT_EQ(std::string(actionProgressLabel(action)), std::format("Turning Efficiency mode {} for", word));
        EXPECT_EQ(formatActionResultMessage(action, 321, Platform::ProcessActionResult::ok()).text,
                  std::format("Efficiency mode {} for PID 321", word));
        EXPECT_EQ(formatActionResultMessage(action, 321, Platform::ProcessActionResult::error("Access denied")).text,
                  std::format("Could not turn Efficiency mode {} for PID 321: Access denied", word));
        const auto authorized = authorizedActionFor(action, target);
        ASSERT_TRUE(authorized.has_value()) << word;
        EXPECT_EQ(authorized->kind, Platform::AuthorizedAction::Kind::SetEfficiencyMode);
        EXPECT_EQ(authorized->efficiencyMode, on);
        EXPECT_EQ(authorized->target, target);
        EXPECT_FALSE(isDestructiveAction(action));
        EXPECT_FALSE(signalFor(action).has_value());
        EXPECT_STREQ(signalName(action), "");
    }
    EXPECT_EQ(mock.efficiencyModeCount(), 2);
    EXPECT_STREQ(actionLabel(ProcessAction::EfficiencyModeOn), "Turn on Efficiency mode"); // the confirm's button
    EXPECT_FALSE(isEfficiencyModeAction(ProcessAction::Kill));

    Domain::ProcessSnapshot snapshot;
    EXPECT_FALSE(isInEfficiencyMode(snapshot));
    snapshot.status = "Efficiency Mode";
    EXPECT_TRUE(isInEfficiencyMode(snapshot));
    snapshot.status = "Suspended";
    EXPECT_FALSE(isInEfficiencyMode(snapshot));
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
    // Exercises the two-call sequence ProcessActionsView and ProcessesPanel run (dispatch, then
    // format), end to end against a mock, without needing to link ProcessDetailsPanel.cpp itself.
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
