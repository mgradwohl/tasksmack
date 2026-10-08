/// @file test_ProcessActionsView.cpp
/// @brief Tests for the Actions tab's state (#1179, slice 3): which buttons the capabilities allow, the
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
    EXPECT_EQ(Detail::ACTION_BUTTON_GRID_COLUMNS, 2U);
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

    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not kill PID 7: Operation not permitted");
}

TEST(ProcessActionsViewTest, NullActionsReportUnavailableInsteadOfCrashing)
{
    ProcessActionsView view;
    view.requestAction(ProcessAction::Terminate, {.pid = 55, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(nullptr);

    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not terminate PID 55: Process actions unavailable");
}

// --- Result line timeout and reset ----------------------------------------------------------------

TEST(ProcessActionsViewTest, ResultStaysUntilItsTimeoutRunsOut)
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.requestAction(ProcessAction::Stop, {.pid = 1, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(&mock);

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
    view.tick(4.0F);

    view.requestAction(ProcessAction::Resume, {.pid = 1, .startTimeTicks = 1}, "proc");
    view.dispatchConfirmed(&mock);
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

        EXPECT_FALSE(view.confirmRequested());
        EXPECT_EQ(view.pendingAction(), ProcessAction::None);
        EXPECT_EQ(view.confirmTarget().target.pid, -1);
        EXPECT_TRUE(view.confirmTarget().processName.empty());
        const std::string firstResult = view.lastResult().text;

        // A second dispatch has nothing to replay: no platform call, and the result line is kept.
        view.dispatchConfirmed(&mock);
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
    view.dispatchConfirmed(nullptr);
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

} // namespace
} // namespace App
