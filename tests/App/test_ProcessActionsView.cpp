/// @file test_ProcessActionsView.cpp
/// @brief Tests for the Actions tab's state (#1179, slice 3): which buttons the capabilities allow, the
/// confirm request, dispatching the confirmed action to a mock IProcessActions, and the result line's
/// timeout and reset. render() is not exercised: ProcessActionsView.cpp draws with ImGui and the theme,
/// and is not linked into TaskSmackTests (like ProcessDetailsPanel.cpp).

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
        view.requestAction(action);
        EXPECT_TRUE(view.confirmRequested());
        EXPECT_EQ(view.pendingAction(), action);
    }
}

TEST(ProcessActionsViewTest, ALaterRequestReplacesTheEarlierOne)
{
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill);
    view.requestAction(ProcessAction::Resume);
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
        view.requestAction(action);
        view.dispatchConfirmed(&mock, {.pid = 321, .startTimeTicks = 99});

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
    view.requestAction(ProcessAction::Kill);
    view.dispatchConfirmed(&mock, {.pid = 7, .startTimeTicks = 1});

    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not kill PID 7: Operation not permitted");
}

TEST(ProcessActionsViewTest, NullActionsReportUnavailableInsteadOfCrashing)
{
    ProcessActionsView view;
    view.requestAction(ProcessAction::Terminate);
    view.dispatchConfirmed(nullptr, {.pid = 55, .startTimeTicks = 1});

    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not terminate PID 55: Process actions unavailable");
}

// --- Result line timeout and reset ----------------------------------------------------------------

TEST(ProcessActionsViewTest, ResultStaysUntilItsTimeoutRunsOut)
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.requestAction(ProcessAction::Stop);
    view.dispatchConfirmed(&mock, {.pid = 1, .startTimeTicks = 1});

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
    view.requestAction(ProcessAction::Stop);
    view.dispatchConfirmed(&mock, {.pid = 1, .startTimeTicks = 1});
    view.tick(4.0F);

    view.requestAction(ProcessAction::Resume);
    view.dispatchConfirmed(&mock, {.pid = 1, .startTimeTicks = 1});
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
    view.requestAction(ProcessAction::Kill);
    view.dispatchConfirmed(&mock, {.pid = 9, .startTimeTicks = 1});
    view.requestAction(ProcessAction::Terminate);
    ASSERT_TRUE(view.confirmRequested());
    ASSERT_FALSE(view.lastResult().empty());

    view.onSelectionChanged();
    EXPECT_FALSE(view.confirmRequested());
    EXPECT_TRUE(view.lastResult().empty());
}

} // namespace
} // namespace App
