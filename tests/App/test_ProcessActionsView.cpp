/// @file test_ProcessActionsView.cpp
/// @brief Tests for the Actions block's state (#1179, slice 3): which buttons the capabilities allow, the
/// confirm request, dispatching the confirmed action to a mock IProcessActions, and the result line's
/// timeout and reset, without an ImGui context. render() and the real modal lifecycle are covered
/// headless in test_ProcessActionConfirmPopup.cpp.

#include "App/Panels/ProcessActionsView.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace App
{
namespace
{

using Detail::ProcessAction;

constexpr std::array<ProcessAction, 4> ALL_ACTIONS{
    ProcessAction::Terminate,
    ProcessAction::Kill,
    ProcessAction::Stop,
    ProcessAction::Resume,
};

// Two different processes: the one a confirm was requested for, and one selected after it.
constexpr Platform::ProcessTarget TARGET_A{.pid = 1001, .startTimeTicks = 5000};
constexpr Platform::ProcessTarget TARGET_B{.pid = 2002, .startTimeTicks = 6000};

/// Capabilities with only @p action's flag set.
Platform::ProcessActionCapabilities onlyCapability(ProcessAction action)
{
    Platform::ProcessActionCapabilities caps;
    caps.canTerminate = action == ProcessAction::Terminate;
    caps.canKill = action == ProcessAction::Kill;
    caps.canStop = action == ProcessAction::Stop;
    caps.canContinue = action == ProcessAction::Resume;
    return caps;
}

// --- Capability gating ---------------------------------------------------------------------------

TEST(ProcessActionsViewTest, EachActionIsGatedByItsOwnCapability)
{
    for (const ProcessAction enabled : ALL_ACTIONS)
    {
        const Platform::ProcessActionCapabilities caps = onlyCapability(enabled);
        for (const ProcessAction action : ALL_ACTIONS)
        {
            SCOPED_TRACE(std::string("enabled=") + Detail::actionLabel(enabled) + " action=" + Detail::actionLabel(action));
            EXPECT_EQ(Detail::isActionAvailable(caps, action), action == enabled);
        }
    }
}

TEST(ProcessActionsViewTest, NothingIsAvailableWithoutCapabilities)
{
    const Platform::ProcessActionCapabilities none;
    for (const ProcessAction action : ALL_ACTIONS)
    {
        SCOPED_TRACE(Detail::actionLabel(action));
        EXPECT_FALSE(Detail::isActionAvailable(none, action));
    }
}

TEST(ProcessActionsViewTest, NoneIsNeverAvailable)
{
    const Platform::ProcessActionCapabilities all{
        .canTerminate = true, .canKill = true, .canStop = true, .canContinue = true, .canSetPriority = true};
    EXPECT_FALSE(Detail::isActionAvailable(all, ProcessAction::None));
}

TEST(ProcessActionsViewTest, SetPriorityAloneEnablesNoButton)
{
    Platform::ProcessActionCapabilities caps;
    caps.canSetPriority = true;
    for (const ProcessAction action : ALL_ACTIONS)
    {
        SCOPED_TRACE(Detail::actionLabel(action));
        EXPECT_FALSE(Detail::isActionAvailable(caps, action));
    }
}

// --- Button table ----------------------------------------------------------------------------------

TEST(ProcessActionsViewTest, ButtonsAreInGridOrderWithTheirLabels)
{
    // Order and labels are the buttons' ImGui IDs and positions in the 2x2 grid: keep them stable.
    constexpr std::array<std::string_view, 4> EXPECTED_SUFFIXES{" Terminate", " Kill", " Suspend", " Resume"};
    ASSERT_EQ(Detail::ACTION_BUTTONS.size(), ALL_ACTIONS.size());
    for (std::size_t i = 0; i < Detail::ACTION_BUTTONS.size(); ++i)
    {
        SCOPED_TRACE(i);
        const Detail::ActionButtonSpec& button = Detail::ACTION_BUTTONS.at(i);
        EXPECT_EQ(button.action, ALL_ACTIONS.at(i));
        EXPECT_TRUE(std::string_view(button.label).ends_with(EXPECTED_SUFFIXES.at(i)));
        // The button, the confirm dialog and the result say the same word (#1203).
        EXPECT_TRUE(std::string_view(button.label).ends_with(Detail::actionLabel(button.action)));
        EXPECT_FALSE(std::string_view(button.tooltip).empty());
    }
}

// --- Confirm request ---------------------------------------------------------------------------------

TEST(ProcessActionsViewTest, StartsWithNothingPending)
{
    const ProcessActionsView view;
    EXPECT_FALSE(view.confirmRequested());
    EXPECT_EQ(view.pendingAction(), ProcessAction::None);
    EXPECT_TRUE(view.lastResult().empty());
}

TEST(ProcessActionsViewTest, RequestingAnActionAsksToConfirmIt)
{
    for (const ProcessAction action : ALL_ACTIONS)
    {
        SCOPED_TRACE(Detail::actionLabel(action));
        ProcessActionsView view;
        view.requestAction(action, TARGET_A, "a");
        EXPECT_TRUE(view.confirmRequested());
        EXPECT_EQ(view.pendingAction(), action);
    }
}

TEST(ProcessActionsViewTest, ALaterRequestReplacesTheEarlierOne)
{
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill, TARGET_A, "a");
    view.requestAction(ProcessAction::Resume, TARGET_A, "a");
    EXPECT_EQ(view.pendingAction(), ProcessAction::Resume);
}

// --- Dispatch ----------------------------------------------------------------------------------------

TEST(ProcessActionsViewTest, ConfirmedActionIsDispatchedToTheTarget)
{
    for (const ProcessAction action : ALL_ACTIONS)
    {
        SCOPED_TRACE(Detail::actionLabel(action));
        TestMocks::MockProcessActions mock;
        ProcessActionsView view;
        view.requestAction(action, {.pid = 321, .startTimeTicks = 99}, "proc");
        view.dispatchConfirmed(&mock);
        view.waitForActionForTesting();

        EXPECT_EQ(mock.terminateCount(), action == ProcessAction::Terminate ? 1 : 0);
        EXPECT_EQ(mock.killCount(), action == ProcessAction::Kill ? 1 : 0);
        EXPECT_EQ(mock.stopCount(), action == ProcessAction::Stop ? 1 : 0);
        EXPECT_EQ(mock.resumeCount(), action == ProcessAction::Resume ? 1 : 0);
        EXPECT_EQ(mock.lastTarget().pid, 321);
        EXPECT_EQ(mock.lastTarget().startTimeTicks, 99U);

        EXPECT_TRUE(view.lastResult().ok);
        EXPECT_EQ(view.lastResult().text, std::string(Detail::actionLabel(action)) + " sent to PID 321");
    }
}

TEST(ProcessActionsViewTest, FailedDispatchShowsTheError)
{
    TestMocks::MockProcessActions mock;
    mock.setKillResult(Platform::ProcessActionResult::error("Operation not permitted"));
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill, {.pid = 7, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();

    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not kill PID 7: Operation not permitted");
}

TEST(ProcessActionsViewTest, NullActionsReportUnavailableInsteadOfCrashing)
{
    ProcessActionsView view;
    view.requestAction(ProcessAction::Terminate, {.pid = 55, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(nullptr);
    view.waitForActionForTesting();

    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not terminate PID 55: Process actions unavailable");
}

// --- Authenticate and retry (#2308) ----------------------------------------------------------------

TEST(ProcessActionsViewTest, APermissionRefusalOffersAuthenticateAndRetryWhereThePlatformCanAsk)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities({.canKill = true, .canAuthorizeInteractively = true});
    mock.setKillResult(Platform::ProcessActionResult::denied("Permission denied - process belongs to another user"));
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill, {.pid = 7, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();
    ASSERT_TRUE(view.offersAuthenticateAndRetry());

    view.retryAuthorizedForTesting();
    view.waitForActionForTesting();
    EXPECT_EQ(mock.runAuthorizedCount(), 1);
    EXPECT_EQ(mock.lastAuthorizedAction().kind, Platform::AuthorizedAction::Kind::Kill);
    EXPECT_EQ(mock.lastAuthorizedAction().target.pid, 7);
    EXPECT_TRUE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Kill sent to PID 7");
    EXPECT_FALSE(view.offersAuthenticateAndRetry());
}

TEST(ProcessActionsViewTest, NoRetryIsOfferedWhereThePlatformCannotAsk)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities({.canKill = true});
    mock.setKillResult(Platform::ProcessActionResult::denied("Permission denied"));
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill, {.pid = 7, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();
    EXPECT_FALSE(view.offersAuthenticateAndRetry());
}

TEST(ProcessActionsViewTest, NoRetryIsOfferedForAFailureOtherThanPermission)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities({.canKill = true, .canAuthorizeInteractively = true});
    mock.setKillResult(Platform::ProcessActionResult::error("Process not found - may have already exited"));
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill, {.pid = 7, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();
    EXPECT_FALSE(view.offersAuthenticateAndRetry());
}

TEST(ProcessActionsViewTest, ACriticalProcessRefusalIsShownWithNoAuthenticateOffer)
{
    // Windows refuses to end a critical system process (#2556) with a plain error: an administrator
    // ending it would stop Windows just the same, so no password is asked for.
    TestMocks::MockProcessActions mock;
    mock.setCapabilities({.canKill = true, .canAuthorizeInteractively = true});
    mock.setKillResult(Platform::ProcessActionResult::error(
        "Process 812 is a critical system process, which Windows can't run without; TaskSmack won't end it"));
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill, {.pid = 812, .startTimeTicks = 1}, "winlogon.exe");
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();

    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text,
              "Could not kill PID 812: Process 812 is a critical system process, which Windows can't run without; TaskSmack won't end it");
    EXPECT_FALSE(view.offersAuthenticateAndRetry());
    EXPECT_EQ(mock.runAuthorizedCount(), 0);
}

TEST(ProcessActionsViewTest, TheRetryOfferGoesWithASelectionChangeOrItsTimeout)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities({.canStop = true, .canAuthorizeInteractively = true});
    mock.setStopResult(Platform::ProcessActionResult::denied("Permission denied"));
    ProcessActionsView view;
    view.requestAction(ProcessAction::Stop, {.pid = 7, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();
    ASSERT_TRUE(view.offersAuthenticateAndRetry());
    view.tick(Detail::ACTION_RESULT_SECONDS + 1.0F); // longer than a plain result: time to decide
    EXPECT_TRUE(view.offersAuthenticateAndRetry());
    view.tick(Detail::AUTHORIZE_RESULT_SECONDS);
    EXPECT_FALSE(view.offersAuthenticateAndRetry());

    view.requestAction(ProcessAction::Stop, {.pid = 7, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();
    ASSERT_TRUE(view.offersAuthenticateAndRetry());
    view.onSelectionChanged();
    EXPECT_FALSE(view.offersAuthenticateAndRetry());
    view.retryAuthorizedForTesting(); // nothing to retry any more
    EXPECT_EQ(mock.runAuthorizedCount(), 0);
}

// #2389: an action that finishes after the selection changed reports to nobody: no result line and no
// retry offer for the process now shown.
TEST(ProcessActionsViewTest, AnActionFinishingAfterASelectionChangeShowsNothing)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities({.canStop = true, .canAuthorizeInteractively = true});
    mock.setStopResult(Platform::ProcessActionResult::denied("Permission denied"));
    ProcessActionsView view;
    view.requestAction(ProcessAction::Stop, {.pid = 7, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(&mock);
    view.onSelectionChanged(); // before its result is taken in
    view.waitForActionForTesting();
    EXPECT_TRUE(view.lastResult().text.empty());
    EXPECT_FALSE(view.offersAuthenticateAndRetry());
}

// #2390: F9 opens no Kill confirm while an action runs, as the Kill button is disabled then.
TEST(ProcessActionsViewTest, KillShortcutWaitsForTheRunningAction)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities({.canKill = true, .canStop = true});
    ProcessActionsView view;
    const Platform::ProcessTarget target{.pid = 7, .startTimeTicks = 1};
    view.requestAction(ProcessAction::Stop, target, "proc");
    view.dispatchConfirmed(&mock);
    ASSERT_TRUE(view.actionRunning());
    EXPECT_FALSE(view.requestKillShortcut(mock.actionCapabilities(), target, "proc"));
    view.waitForActionForTesting();
    EXPECT_TRUE(view.requestKillShortcut(mock.actionCapabilities(), target, "proc"));
}

TEST(ProcessActionsViewTest, ADismissedPromptIsTheResultAndIsNotOfferedAgain)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities({.canKill = true, .canAuthorizeInteractively = true});
    mock.setKillResult(Platform::ProcessActionResult::denied("Permission denied"));
    mock.setRunAuthorizedResult(
        Platform::ProcessActionResult::error("Couldn't act as an administrator: the password prompt was dismissed"));
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill, {.pid = 7, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();
    view.retryAuthorizedForTesting();
    view.waitForActionForTesting();
    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not kill PID 7: Couldn't act as an administrator: the password prompt was dismissed");
    EXPECT_FALSE(view.offersAuthenticateAndRetry());
}

// --- Result line timeout and reset ----------------------------------------------------------------

TEST(ProcessActionsViewTest, ResultStaysUntilItsTimeoutRunsOut)
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.requestAction(ProcessAction::Stop, {.pid = 1, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();

    view.tick(Detail::ACTION_RESULT_SECONDS - 1.0F);
    EXPECT_FALSE(view.lastResult().empty());
    view.tick(1.0F);
    EXPECT_TRUE(view.lastResult().empty());
}

TEST(ProcessActionsViewTest, ResultTimeoutIsFiveSeconds)
{
    EXPECT_FLOAT_EQ(Detail::ACTION_RESULT_SECONDS, 5.0F);
}

TEST(ProcessActionsViewTest, ANewResultRestartsTheTimeout)
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.requestAction(ProcessAction::Stop, {.pid = 1, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();
    view.tick(4.0F);

    view.requestAction(ProcessAction::Resume, {.pid = 1, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();
    view.tick(4.0F);
    EXPECT_EQ(view.lastResult().text, "Resume sent to PID 1");
}

TEST(ProcessActionsViewTest, TickWithoutAResultDoesNothing)
{
    ProcessActionsView view;
    view.tick(10.0F);
    EXPECT_TRUE(view.lastResult().empty());
    EXPECT_FALSE(view.confirmRequested());
}

TEST(ProcessActionsViewTest, SelectionChangeClosesTheDialogAndDropsTheResult)
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill, {.pid = 9, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();
    view.requestAction(ProcessAction::Terminate, TARGET_A, "a");
    ASSERT_TRUE(view.confirmRequested());
    ASSERT_FALSE(view.lastResult().empty());

    view.onSelectionChanged();
    EXPECT_FALSE(view.confirmRequested());
    EXPECT_TRUE(view.lastResult().empty());
}

// --- Captured confirm target (Copilot review on #1447) -----------------------------------------------

TEST(ProcessActionsViewTest, RequestCapturesTheTargetAndName)
{
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill, TARGET_A, "firefox");
    EXPECT_EQ(view.confirmTarget().target.pid, TARGET_A.pid);
    EXPECT_EQ(view.confirmTarget().target.startTimeTicks, TARGET_A.startTimeTicks);
    EXPECT_EQ(view.confirmTarget().processName, "firefox");
}

TEST(ProcessActionsViewTest, ConfirmActsOnTheCapturedTargetNotALaterOne)
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill, TARGET_A, "a");
    // The selection moved to B without a selection-change notice: the dialog must be dismissed,
    // and a confirm that slipped through reaches no platform call, least of all for B.
    EXPECT_TRUE(view.takeDismiss(TARGET_B));
    EXPECT_FALSE(view.confirmRequested());
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();
    EXPECT_EQ(mock.killCount(), 0);
    EXPECT_NE(mock.lastTarget().pid, TARGET_B.pid);
}

TEST(ProcessActionsViewTest, SelectionChangeCancelsThePendingConfirm)
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.requestAction(ProcessAction::Terminate, TARGET_A, "a");

    view.onSelectionChanged(); // Now B is selected
    EXPECT_FALSE(view.confirmRequested());
    EXPECT_EQ(view.pendingAction(), ProcessAction::None);
    EXPECT_EQ(view.confirmTarget().target.pid, -1);
    // The next render closes the dialog ImGui may still have open, whichever process is live.
    EXPECT_TRUE(view.takeDismiss(TARGET_B));
    EXPECT_FALSE(view.takeDismiss(TARGET_B)); // Consumed

    // A confirm now reaches no platform call at all, for A or B, and shows no result.
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();
    EXPECT_EQ(mock.terminateCount(), 0);
    EXPECT_EQ(mock.killCount(), 0);
    EXPECT_EQ(mock.stopCount(), 0);
    EXPECT_EQ(mock.resumeCount(), 0);
    EXPECT_TRUE(view.lastResult().empty());
}

// --- F9 (#170) ----------------------------------------------------------------------------------------

constexpr Platform::ProcessActionCapabilities KILL_ONLY{.canKill = true};

TEST(ProcessActionsViewTest, KillShortcutAllowedNeedsCapabilityTargetAndNothingPending)
{
    EXPECT_TRUE(Detail::killShortcutAllowed(KILL_ONLY, TARGET_A, false));
    EXPECT_FALSE(Detail::killShortcutAllowed(KILL_ONLY, TARGET_A, true)); // A confirm is pending
    EXPECT_FALSE(Detail::killShortcutAllowed({.canTerminate = true}, TARGET_A, false));
    EXPECT_FALSE(Detail::killShortcutAllowed(KILL_ONLY, {.pid = -1, .startTimeTicks = 0}, false));
    EXPECT_FALSE(Detail::killShortcutAllowed(KILL_ONLY, {.pid = 0, .startTimeTicks = 0}, false));
}

TEST(ProcessActionsViewTest, KillShortcutNeverReplacesAPendingConfirm)
{
    ProcessActionsView view;
    view.requestAction(ProcessAction::Stop, TARGET_A, "a");
    EXPECT_FALSE(view.requestKillShortcut(KILL_ONLY, TARGET_B, "b"));
    EXPECT_EQ(view.pendingAction(), ProcessAction::Stop);
    EXPECT_EQ(view.confirmTarget().target.pid, TARGET_A.pid);
    EXPECT_EQ(view.confirmTarget().processName, "a");
}

TEST(ProcessActionsViewTest, SelectionChangeWithNothingPendingQueuesNoDismiss)
{
    ProcessActionsView view;
    view.onSelectionChanged();
    EXPECT_FALSE(view.takeDismiss(TARGET_B));

    // And a dismissal queued earlier is dropped by a new F9 request, which it must not close.
    view.requestAction(ProcessAction::Stop, TARGET_A, "a");
    view.onSelectionChanged();
    ASSERT_TRUE(view.requestKillShortcut(KILL_ONLY, TARGET_B, "b"));
    EXPECT_FALSE(view.takeDismiss(TARGET_B));
    EXPECT_TRUE(view.confirmRequested());
    EXPECT_EQ(view.pendingAction(), ProcessAction::Kill);
}

TEST(ProcessActionsViewTest, SameTargetKeepsTheDialogOpen)
{
    ProcessActionsView view;
    view.requestAction(ProcessAction::Stop, TARGET_A, "a");
    EXPECT_FALSE(view.takeDismiss(TARGET_A));
    EXPECT_TRUE(view.confirmRequested());
    EXPECT_EQ(view.pendingAction(), ProcessAction::Stop);
}

TEST(ProcessActionsViewTest, NoDismissWithNothingPending)
{
    ProcessActionsView view;
    EXPECT_FALSE(view.takeDismiss(TARGET_B));
}

TEST(ProcessActionsViewTest, ANewRequestAfterASelectionChangeTargetsTheNewProcess)
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill, TARGET_A, "a");
    view.onSelectionChanged();
    ASSERT_TRUE(view.takeDismiss(TARGET_B));

    view.requestAction(ProcessAction::Kill, TARGET_B, "b");
    EXPECT_FALSE(view.takeDismiss(TARGET_B));
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();
    EXPECT_EQ(mock.killCount(), 1);
    EXPECT_EQ(mock.lastTarget().pid, TARGET_B.pid);
}

TEST(ProcessActionsViewTest, SameProcessTargetComparesPidAndKnownStartTime)
{
    struct Case
    {
        const char* name = "";
        Platform::ProcessTarget captured;
        Platform::ProcessTarget live;
        bool same = false;
    };
    const std::array<Case, 5> cases{{
        {.name = "identical", .captured = TARGET_A, .live = TARGET_A, .same = true},
        {.name = "other pid", .captured = TARGET_A, .live = TARGET_B, .same = false},
        {.name = "reused pid", .captured = TARGET_A, .live = {.pid = TARGET_A.pid, .startTimeTicks = 9999}, .same = false},
        {.name = "captured start unknown", .captured = {.pid = TARGET_A.pid, .startTimeTicks = 0}, .live = TARGET_A, .same = true},
        {.name = "live start unknown", .captured = TARGET_A, .live = {.pid = TARGET_A.pid, .startTimeTicks = 0}, .same = true},
    }};
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.name);
        EXPECT_EQ(Detail::isSameProcessTarget(c.captured, c.live), c.same);
    }
}

// --- Nothing stale after the dialog closes (Copilot review on #1447) --------------------------------

TEST(ProcessActionsViewTest, ConfirmClearsThePendingConfirmAndRunsOnce)
{
    for (const ProcessAction action : ALL_ACTIONS)
    {
        SCOPED_TRACE(Detail::actionLabel(action));
        TestMocks::MockProcessActions mock;
        ProcessActionsView view;
        view.requestAction(action, TARGET_A, "a");
        view.dispatchConfirmed(&mock);
        view.waitForActionForTesting();

        EXPECT_FALSE(view.confirmRequested());
        EXPECT_EQ(view.pendingAction(), ProcessAction::None);
        EXPECT_EQ(view.confirmTarget().target.pid, -1);
        EXPECT_TRUE(view.confirmTarget().processName.empty());
        const std::string firstResult = view.lastResult().text;

        // A second dispatch has nothing to replay: no platform call, and the result line is kept.
        view.dispatchConfirmed(&mock);
        view.waitForActionForTesting();
        const int calls = mock.terminateCount() + mock.killCount() + mock.stopCount() + mock.resumeCount();
        EXPECT_EQ(calls, 1);
        EXPECT_EQ(view.lastResult().text, firstResult);
    }
}

TEST(ProcessActionsViewTest, CancelClearsThePendingConfirmAndDispatchesNothing)
{
    for (const ProcessAction action : ALL_ACTIONS)
    {
        SCOPED_TRACE(Detail::actionLabel(action));
        TestMocks::MockProcessActions mock;
        ProcessActionsView view;
        view.requestAction(action, TARGET_A, "a");
        view.cancelConfirm();

        EXPECT_FALSE(view.confirmRequested());
        EXPECT_EQ(view.pendingAction(), ProcessAction::None);
        EXPECT_EQ(view.confirmTarget().target.pid, -1);
        EXPECT_TRUE(view.confirmTarget().processName.empty());

        view.dispatchConfirmed(&mock);
        view.waitForActionForTesting();
        const int calls = mock.terminateCount() + mock.killCount() + mock.stopCount() + mock.resumeCount();
        EXPECT_EQ(calls, 0);
        EXPECT_TRUE(view.lastResult().empty());
    }
}

TEST(ProcessActionsViewTest, DispatchWithNothingPendingDoesNothing)
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();
    view.dispatchConfirmed(nullptr);
    view.waitForActionForTesting();
    EXPECT_EQ(mock.terminateCount() + mock.killCount() + mock.stopCount() + mock.resumeCount(), 0);
    EXPECT_TRUE(view.lastResult().empty());
}

TEST(ProcessActionsViewTest, DismissForAMovedTargetClearsThePendingConfirm)
{
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill, TARGET_A, "a");
    ASSERT_TRUE(view.takeDismiss(TARGET_B));
    EXPECT_EQ(view.pendingAction(), ProcessAction::None);
    EXPECT_EQ(view.confirmTarget().target.pid, -1);
}

// --- Trace system calls (#182) ---------------------------------------------------------------------

/// Capabilities with only the trace availability set.
constexpr Platform::ProcessActionCapabilities traceCapability(Platform::SyscallTraceAvailability availability)
{
    return {.syscallTrace = availability};
}

TEST(ProcessActionsViewTest, TraceButtonIsHiddenWhereThePlatformHasNoTracer)
{
    // Windows, and the synthetic scenario: the default capabilities say Unsupported.
    EXPECT_EQ(Detail::syscallTraceButton(Platform::ProcessActionCapabilities{}, TARGET_A).state, Detail::SyscallTraceButtonState::Hidden);
    EXPECT_EQ(Detail::syscallTraceButton(traceCapability(Platform::SyscallTraceAvailability::Unsupported), TARGET_A).state,
              Detail::SyscallTraceButtonState::Hidden);
}

TEST(ProcessActionsViewTest, TraceButtonIsDisabledWithTheReasonWhenAToolIsMissing)
{
    for (const auto availability : {Platform::SyscallTraceAvailability::NoTracer, Platform::SyscallTraceAvailability::NoTerminal})
    {
        SCOPED_TRACE(static_cast<int>(availability));
        const Detail::SyscallTraceButton button = Detail::syscallTraceButton(traceCapability(availability), TARGET_A);
        EXPECT_EQ(button.state, Detail::SyscallTraceButtonState::Disabled);
        EXPECT_STREQ(button.tooltip, Platform::syscallTraceUnavailableReason(availability));
        EXPECT_FALSE(std::string_view(button.tooltip).empty());
    }
    EXPECT_NE(std::string_view(Platform::syscallTraceUnavailableReason(Platform::SyscallTraceAvailability::NoTracer)).find("strace"),
              std::string_view::npos);
    EXPECT_NE(std::string_view(Platform::syscallTraceUnavailableReason(Platform::SyscallTraceAvailability::NoTerminal)).find("TERMINAL"),
              std::string_view::npos);
}

TEST(ProcessActionsViewTest, TraceButtonIsEnabledOnlyWithATarget)
{
    const auto caps = traceCapability(Platform::SyscallTraceAvailability::Available);
    const Detail::SyscallTraceButton enabled = Detail::syscallTraceButton(caps, TARGET_A);
    EXPECT_EQ(enabled.state, Detail::SyscallTraceButtonState::Enabled);
    EXPECT_FALSE(std::string_view(enabled.tooltip).empty());
    EXPECT_EQ(Detail::syscallTraceButton(caps, {.pid = 0, .startTimeTicks = 0}).state, Detail::SyscallTraceButtonState::Disabled);
    EXPECT_EQ(Detail::syscallTraceButton(caps, {.pid = -1, .startTimeTicks = 0}).state, Detail::SyscallTraceButtonState::Disabled);
    EXPECT_TRUE(std::string_view(Detail::SYSCALL_TRACE_LABEL).ends_with("Trace system calls (strace)"));
}

TEST(ProcessActionsViewTest, TraceLaunchesOnTheTargetAndShowsTheResult)
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.launchSyscallTrace(&mock, TARGET_A);
    view.waitForActionForTesting();
    EXPECT_EQ(mock.syscallTraceCount(), 1);
    EXPECT_EQ(mock.lastTarget().pid, TARGET_A.pid);
    EXPECT_EQ(mock.lastTarget().startTimeTicks, TARGET_A.startTimeTicks);
    EXPECT_TRUE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Opened strace for PID 1001 in a new terminal");
    // No confirm is involved, and no other action is sent.
    EXPECT_FALSE(view.confirmPending());
    EXPECT_EQ(mock.terminateCount() + mock.killCount() + mock.stopCount() + mock.resumeCount(), 0);
}

TEST(ProcessActionsViewTest, TraceFailureShowsThePlatformMessage)
{
    TestMocks::MockProcessActions mock;
    mock.setSyscallTraceResult(Platform::ProcessActionResult::error("ptrace is limited (/proc/sys/kernel/yama/ptrace_scope is 1)"));
    ProcessActionsView view;
    view.launchSyscallTrace(&mock, TARGET_B);
    view.waitForActionForTesting();
    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not trace PID 2002: ptrace is limited (/proc/sys/kernel/yama/ptrace_scope is 1)");

    // The result line times out like any other.
    view.tick(Detail::ACTION_RESULT_SECONDS + 0.1F);
    EXPECT_TRUE(view.lastResult().empty());
}

TEST(ProcessActionsViewTest, TraceWithoutActionsReportsUnavailable)
{
    ProcessActionsView view;
    view.launchSyscallTrace(nullptr, TARGET_A);
    view.waitForActionForTesting();
    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not trace PID 1001: Process actions unavailable");
}

/// An IProcessActions that does not override launchSyscallTrace(), as Windows' and the synthetic one.
class NoTraceActions : public Platform::IProcessActions
{
  public:
    [[nodiscard]] Platform::ProcessActionCapabilities actionCapabilities() const override
    {
        return {};
    }
    [[nodiscard]] Platform::ProcessActionResult terminate(const Platform::ProcessTarget& /*target*/) override
    {
        return Platform::ProcessActionResult::ok();
    }
    [[nodiscard]] Platform::ProcessActionResult kill(const Platform::ProcessTarget& /*target*/) override
    {
        return Platform::ProcessActionResult::ok();
    }
    [[nodiscard]] Platform::ProcessActionResult stop(const Platform::ProcessTarget& /*target*/) override
    {
        return Platform::ProcessActionResult::ok();
    }
    [[nodiscard]] Platform::ProcessActionResult resume(const Platform::ProcessTarget& /*target*/) override
    {
        return Platform::ProcessActionResult::ok();
    }
    [[nodiscard]] Platform::ProcessActionResult setPriority(const Platform::ProcessTarget& /*target*/, std::int32_t /*nice*/) override
    {
        return Platform::ProcessActionResult::ok();
    }
    [[nodiscard]] Platform::ProcessActionResult
    setIoPriority(const Platform::ProcessTarget& /*target*/, Platform::IoPriorityClass /*ioClass*/, std::int32_t /*level*/) override
    {
        return Platform::ProcessActionResult::ok();
    }
    [[nodiscard]] Platform::IoPriorityReadResult getIoPriority(const Platform::ProcessTarget& /*target*/) override
    {
        return Platform::IoPriority{};
    }
};

// --- Efficiency mode (#3071) ---------------------------------------------------------------------------

TEST(ProcessActionsViewTest, TheEfficiencyModeToggleIsHiddenDisabledOrEnabledAndSaysWhy)
{
    const Platform::ProcessActionCapabilities none;
    const Platform::ProcessActionCapabilities can{.canSetEfficiencyMode = true};
    using State = Detail::EfficiencyModeToggleState;
    EXPECT_EQ(Detail::efficiencyModeToggle(none, TARGET_A, false, false).state, State::Hidden) << "Linux: no such toggle";
    EXPECT_TRUE(Detail::isActionAvailable(can, ProcessAction::EfficiencyModeOn));
    EXPECT_FALSE(Detail::isActionAvailable(none, ProcessAction::EfficiencyModeOff));

    const auto disabledBecause = [&can](const Platform::ProcessTarget& target, bool running, std::size_t batch, std::string_view why)
    {
        const Detail::EfficiencyModeToggle toggle = Detail::efficiencyModeToggle(can, target, false, running, batch);
        EXPECT_EQ(toggle.state, State::Disabled) << why;
        EXPECT_TRUE(std::string_view(toggle.tooltip).contains(why)) << toggle.tooltip;
    };
    disabledBecause(TARGET_A, false, 3, "one process at a time");
    disabledBecause({.pid = -1, .startTimeTicks = 0}, false, 0, "Select a process");
    disabledBecause({.pid = 1001, .startTimeTicks = 0}, false, 0, "can't confirm which process");
    disabledBecause(TARGET_A, true, 0, "Another action is still running");

    const Detail::EfficiencyModeToggle off = Detail::efficiencyModeToggle(can, TARGET_A, false, false);
    EXPECT_EQ(off.state, State::Enabled);
    EXPECT_TRUE(std::string_view(off.tooltip).contains("Idle priority")) << off.tooltip;
    const Detail::EfficiencyModeToggle on = Detail::efficiencyModeToggle(can, TARGET_A, true, false, 1);
    EXPECT_EQ(on.state, State::Enabled);
    EXPECT_TRUE(std::string_view(on.tooltip).contains("Turn Efficiency mode off")) << on.tooltip;
}

TEST(ProcessActionsViewTest, TurningEfficiencyModeOnAsksFirstAndOffRunsAtOnce)
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.requestEfficiencyModeToggle(&mock, TARGET_A, "chrome.exe", /*on=*/false);
    EXPECT_TRUE(view.confirmRequested());
    EXPECT_EQ(view.pendingAction(), ProcessAction::EfficiencyModeOn);
    EXPECT_EQ(mock.efficiencyModeCount(), 0) << "asked, not done";
    view.dispatchConfirmed(&mock);
    view.waitForActionForTesting();
    EXPECT_EQ(mock.efficiencyModeCount(), 1);
    EXPECT_TRUE(mock.lastEfficiencyMode());
    EXPECT_EQ(view.lastResult().text, "Efficiency mode on for PID 1001");

    view.requestEfficiencyModeToggle(&mock, TARGET_A, "chrome.exe", /*on=*/true);
    EXPECT_FALSE(view.confirmRequested()) << "turning it off isn't confirmed";
    view.waitForActionForTesting();
    EXPECT_EQ(mock.efficiencyModeCount(), 2);
    EXPECT_FALSE(mock.lastEfficiencyMode());
    EXPECT_EQ(mock.lastTarget(), TARGET_A);
    EXPECT_EQ(view.lastResult().text, "Efficiency mode off for PID 1001");
}

TEST(ProcessActionsViewTest, ARefusedEfficiencyModeOffersAuthenticateAndRetry)
{
    TestMocks::MockProcessActions mock;
    mock.setCapabilities({.canAuthorizeInteractively = true, .canSetEfficiencyMode = true});
    mock.setEfficiencyModeResult(Platform::ProcessActionResult::denied("Access denied"));
    ProcessActionsView view;
    view.requestEfficiencyModeToggle(&mock, TARGET_A, "chrome.exe", /*on=*/true);
    view.waitForActionForTesting();
    EXPECT_EQ(view.lastResult().text, "Could not turn Efficiency mode off for PID 1001: Access denied");
    ASSERT_TRUE(view.offersAuthenticateAndRetry());

    view.retryAuthorizedForTesting();
    view.waitForActionForTesting();
    EXPECT_EQ(mock.lastAuthorizedAction().kind, Platform::AuthorizedAction::Kind::SetEfficiencyMode);
    EXPECT_FALSE(mock.lastAuthorizedAction().efficiencyMode);
    EXPECT_EQ(mock.lastAuthorizedAction().target, TARGET_A);
    EXPECT_EQ(view.lastResult().text, "Efficiency mode off for PID 1001");
}

TEST(ProcessActionsViewTest, TheInterfaceDefaultReportsTracingUnsupported)
{
    NoTraceActions actions;
    EXPECT_EQ(actions.actionCapabilities().syscallTrace, Platform::SyscallTraceAvailability::Unsupported);
    const Platform::ProcessActionResult result = actions.launchSyscallTrace(TARGET_A);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, Platform::syscallTraceUnavailableReason(Platform::SyscallTraceAvailability::Unsupported));
}

} // namespace
} // namespace App
