/// @file test_ProcessPriorityView.cpp
/// @brief Tests for the priority control's state (#1179, slice 4): the nice range and clamping, Apply's
/// enablement, applying to a mock IProcessActions with the target's PID and start time, the error line,
/// the reset on a selection change, and that an apply is never replayed or sent to another process,
/// without an ImGui context. render() itself is covered headless in test_ProcessPriorityViewRender.cpp.

#include "App/Panels/ProcessDetailsPanel_PriorityHelpers.h"
#include "App/Panels/ProcessPriorityView.h"
#include "Domain/PriorityConfig.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace App
{
namespace
{

// Two different processes, and a later process given A's PID.
constexpr Platform::ProcessTarget TARGET_A{.pid = 1001, .startTimeTicks = 5000};
constexpr Platform::ProcessTarget TARGET_B{.pid = 2002, .startTimeTicks = 6000};
constexpr Platform::ProcessTarget TARGET_A_REUSED{.pid = 1001, .startTimeTicks = 9000};
// A before its first snapshot: the PID alone, start time not yet known.
constexpr Platform::ProcessTarget TARGET_A_UNCONFIRMED{.pid = 1001, .startTimeTicks = 0};

constexpr std::optional<std::int32_t> NICE_OF_A = 0;

// --- Range, clamping and Apply enablement --------------------------------------------------------

TEST(ProcessPriorityViewTest, TheRangeIsTheUnixNiceRange)
{
    EXPECT_EQ(Detail::NICE_MIN, -20);
    EXPECT_EQ(Detail::NICE_MAX, 19);
    EXPECT_EQ(Detail::getNiceFromPosition(0.0F), Detail::NICE_MIN);
    EXPECT_EQ(Detail::getNiceFromPosition(1.0F), Detail::NICE_MAX);
}

TEST(ProcessPriorityViewTest, StepNiceHoldsToTheRange)
{
    struct Case
    {
        std::int32_t current;
        std::int32_t delta;
        std::int32_t expected;
    };
    // The slider's Left/Right (±1) and PgUp/PgDown (±5) keys, in range and against both ends.
    constexpr std::array<Case, 10> CASES{{
        {.current = 0, .delta = -1, .expected = -1},
        {.current = 0, .delta = 1, .expected = 1},
        {.current = 0, .delta = -5, .expected = -5},
        {.current = 0, .delta = 5, .expected = 5},
        {.current = -20, .delta = -1, .expected = -20},
        {.current = -18, .delta = -5, .expected = -20},
        {.current = 19, .delta = 1, .expected = 19},
        {.current = 16, .delta = 5, .expected = 19},
        {.current = -20, .delta = 5, .expected = -15},
        {.current = 19, .delta = -5, .expected = 14},
    }};
    for (const Case& c : CASES)
    {
        SCOPED_TRACE("current=" + std::to_string(c.current) + " delta=" + std::to_string(c.delta));
        EXPECT_EQ(Detail::stepNice(c.current, c.delta), c.expected);
    }
}

TEST(ProcessPriorityViewTest, AnEditIsHeldToTheRange)
{
    struct Case
    {
        std::int32_t requested;
        std::int32_t expected;
    };
    constexpr std::array<Case, 6> CASES{{
        {.requested = 100, .expected = 19},
        {.requested = 20, .expected = 19},
        {.requested = 19, .expected = 19},
        {.requested = -20, .expected = -20},
        {.requested = -21, .expected = -20},
        {.requested = -100, .expected = -20},
    }};
    for (const Case& c : CASES)
    {
        SCOPED_TRACE("requested=" + std::to_string(c.requested));
        ProcessPriorityView view;
        view.editNice(c.requested, TARGET_A);
        EXPECT_EQ(view.niceValue(), c.expected);
        EXPECT_TRUE(view.hasPendingEdit());
    }
}

TEST(ProcessPriorityViewTest, ApplyIsDisabledUntilAnEdit)
{
    ProcessPriorityView view;
    view.syncToProcess(NICE_OF_A);
    EXPECT_FALSE(view.canApply(NICE_OF_A, TARGET_A));

    view.editNice(5, TARGET_A);
    EXPECT_TRUE(view.canApply(NICE_OF_A, TARGET_A));
}

TEST(ProcessPriorityViewTest, ApplyIsDisabledWithoutASnapshot)
{
    ProcessPriorityView view;
    view.editNice(5, TARGET_A_UNCONFIRMED);
    EXPECT_TRUE(view.hasPendingEdit());
    EXPECT_FALSE(view.canApply(std::nullopt, TARGET_A_UNCONFIRMED));
}

TEST(ProcessPriorityViewTest, PickingTheShownValueIsNoEdit)
{
    ProcessPriorityView view;
    view.syncToProcess(3);
    view.editNice(3, TARGET_A);
    EXPECT_FALSE(view.hasPendingEdit());
    EXPECT_FALSE(view.canApply(3, TARGET_A));
    EXPECT_EQ(view.editTarget().pid, -1);
}

TEST(ProcessPriorityViewTest, TheControlFollowsTheProcessUntilEdited)
{
    ProcessPriorityView view;
    view.syncToProcess(4);
    EXPECT_EQ(view.niceValue(), 4);
    view.syncToProcess(std::nullopt); // No snapshot: the value stays
    EXPECT_EQ(view.niceValue(), 4);

    view.editNice(10, TARGET_A);
    view.syncToProcess(4); // An edit is not overwritten by the process's own value
    EXPECT_EQ(view.niceValue(), 10);
}

TEST(ProcessPriorityViewTest, AnEditRemembersItsTarget)
{
    ProcessPriorityView view;
    view.editNice(7, TARGET_A);
    EXPECT_EQ(view.editTarget().pid, TARGET_A.pid);
    EXPECT_EQ(view.editTarget().startTimeTicks, TARGET_A.startTimeTicks);
}

// --- Apply ---------------------------------------------------------------------------------------

TEST(ProcessPriorityViewTest, ApplySetsTheEditOnTheTargetWithItsStartTime)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    view.editNice(10, TARGET_A);
    view.apply(&mock, TARGET_A, NICE_OF_A);

    EXPECT_EQ(mock.setPriorityCount(), 1);
    EXPECT_EQ(mock.lastSetPriorityPid(), TARGET_A.pid);
    EXPECT_EQ(mock.lastTarget().startTimeTicks, TARGET_A.startTimeTicks);
    EXPECT_EQ(mock.lastSetPriorityNice(), 10);
}

TEST(ProcessPriorityViewTest, AnEditBeforeTheStartTimeIsKnownIsDroppedOnceItIs)
{
    // Edited while only the PID was known; by Apply a snapshot knows the start time. The PID may have
    // been reused in between, and a check of the live target cannot tell, so the edit is dropped with
    // no platform call (Copilot review on #1455), for A's own start time or a reuse's alike.
    for (const Platform::ProcessTarget& live : {TARGET_A, TARGET_A_REUSED})
    {
        SCOPED_TRACE("start=" + std::to_string(live.startTimeTicks));
        TestMocks::MockProcessActions mock;
        ProcessPriorityView view;
        view.editNice(10, TARGET_A_UNCONFIRMED);
        view.apply(&mock, live, NICE_OF_A);

        EXPECT_EQ(mock.setPriorityCount(), 0);
        EXPECT_FALSE(view.hasPendingEdit());
        EXPECT_EQ(view.editTarget().pid, -1);
    }
}

TEST(ProcessPriorityViewTest, ApplyIsDisabledAndSendsNothingWhileTheStartTimeIsUnknown)
{
    // Every IProcessActions refuses a start time of 0 (checkProcessIdentity()), so Apply is not offered
    // and apply() makes no call while it is unknown, even with a snapshot's nice value in hand
    // (Copilot review on #1455). The slider can still be moved; the edit waits, and the tooltip says so.
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    view.editNice(10, TARGET_A_UNCONFIRMED);
    EXPECT_FALSE(view.canApply(NICE_OF_A, TARGET_A_UNCONFIRMED));
    EXPECT_TRUE(view.waitingForProcessDetails(NICE_OF_A, TARGET_A_UNCONFIRMED));

    view.apply(&mock, TARGET_A_UNCONFIRMED, NICE_OF_A);
    EXPECT_EQ(mock.setPriorityCount(), 0);
    EXPECT_TRUE(view.hasPendingEdit()); // Kept until the start time is known, then dropped

    // A known edit whose live target lost its start time is not sent either.
    ProcessPriorityView known;
    known.editNice(10, TARGET_A);
    EXPECT_FALSE(known.canApply(NICE_OF_A, TARGET_A_UNCONFIRMED));
    known.apply(&mock, TARGET_A_UNCONFIRMED, NICE_OF_A);
    EXPECT_EQ(mock.setPriorityCount(), 0);
}

TEST(ProcessPriorityViewTest, NotWaitingWithoutAnEditOrOnceApplicable)
{
    ProcessPriorityView view;
    EXPECT_FALSE(view.waitingForProcessDetails(NICE_OF_A, TARGET_A_UNCONFIRMED)); // Nothing edited
    view.editNice(10, TARGET_A);
    EXPECT_FALSE(view.waitingForProcessDetails(NICE_OF_A, TARGET_A));
    EXPECT_TRUE(view.waitingForProcessDetails(std::nullopt, TARGET_A)); // No snapshot yet
}

TEST(ProcessPriorityViewTest, SameEditTargetRules)
{
    struct Case
    {
        Platform::ProcessTarget edited;
        Platform::ProcessTarget live;
        bool same = false;    // The edit is kept (isSameEditTarget)
        bool applies = false; // ...and Apply may send it (canApply): only with a known, equal start time
        const char* name = "";
    };
    const std::array<Case, 6> cases{{
        {.edited = TARGET_A, .live = TARGET_A, .same = true, .applies = true, .name = "both known, equal"},
        {.edited = TARGET_A_UNCONFIRMED, .live = TARGET_A_UNCONFIRMED, .same = true, .applies = false, .name = "both unknown"},
        {.edited = TARGET_A, .live = TARGET_A_UNCONFIRMED, .same = true, .applies = false, .name = "live lost its start time"},
        {.edited = TARGET_A_UNCONFIRMED, .live = TARGET_A, .same = false, .applies = false, .name = "unknown became known"},
        {.edited = TARGET_A, .live = TARGET_A_REUSED, .same = false, .applies = false, .name = "PID reused"},
        {.edited = TARGET_A, .live = TARGET_B, .same = false, .applies = false, .name = "different PID"},
    }};
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.name);
        EXPECT_EQ(Detail::isSameEditTarget(c.edited, c.live), c.same);

        TestMocks::MockProcessActions mock;
        ProcessPriorityView view;
        view.editNice(10, c.edited);
        // canApply() on its own, with no dropEditIfTargetMoved() first: it must not rely on one.
        EXPECT_EQ(view.canApply(NICE_OF_A, c.live), c.applies);
        view.apply(&mock, c.live, NICE_OF_A);
        EXPECT_EQ(mock.setPriorityCount(), c.applies ? 1 : 0);
    }
}

TEST(ProcessPriorityViewTest, CanApplyIsFalseForAnotherKnownProcessWithoutADropFirst)
{
    // Copilot review on #1455: canApply() is self-contained, not dependent on the caller having
    // dropped a moved edit first, as render() does.
    for (const Platform::ProcessTarget& other : {TARGET_B, TARGET_A_REUSED})
    {
        SCOPED_TRACE("pid=" + std::to_string(other.pid) + " start=" + std::to_string(other.startTimeTicks));
        TestMocks::MockProcessActions mock;
        ProcessPriorityView view;
        view.editNice(10, TARGET_A);
        EXPECT_FALSE(view.canApply(NICE_OF_A, other));
        EXPECT_TRUE(view.canApply(NICE_OF_A, TARGET_A)); // Still pending for A: canApply() changes nothing

        view.apply(&mock, other, NICE_OF_A);
        EXPECT_EQ(mock.setPriorityCount(), 0);
    }
}

TEST(ProcessPriorityViewTest, SuccessClearsTheErrorAndEndsTheEdit)
{
    TestMocks::MockProcessActions mock;
    mock.setPriorityResult(Platform::ProcessActionResult::error("Permission denied"));
    ProcessPriorityView view;
    view.editNice(-5, TARGET_A);
    view.apply(&mock, TARGET_A, NICE_OF_A);
    ASSERT_EQ(view.error(), "Permission denied");

    mock.setPriorityResult(Platform::ProcessActionResult::ok());
    view.editNice(5, TARGET_A);
    view.apply(&mock, TARGET_A, NICE_OF_A);

    EXPECT_TRUE(view.error().empty());
    EXPECT_FALSE(view.hasPendingEdit());
    EXPECT_EQ(view.niceValue(), 5); // Shown until the next snapshot's value takes over
    EXPECT_EQ(view.editTarget().pid, -1);
}

TEST(ProcessPriorityViewTest, FailureShowsThePlatformMessageAndRevertsTheControl)
{
    TestMocks::MockProcessActions mock;
    mock.setPriorityResult(Platform::ProcessActionResult::error("Permission denied"));
    ProcessPriorityView view;
    view.syncToProcess(2);
    view.editNice(-10, TARGET_A);
    view.apply(&mock, TARGET_A, 2);

    EXPECT_EQ(view.error(), "Permission denied");
    EXPECT_EQ(view.niceValue(), 2);
    EXPECT_FALSE(view.hasPendingEdit());
    EXPECT_EQ(view.editTarget().pid, -1);
}

TEST(ProcessPriorityViewTest, AnotherEditClearsTheError)
{
    TestMocks::MockProcessActions mock;
    mock.setPriorityResult(Platform::ProcessActionResult::error("Permission denied"));
    ProcessPriorityView view;
    view.editNice(-10, TARGET_A);
    view.apply(&mock, TARGET_A, NICE_OF_A);
    ASSERT_FALSE(view.error().empty());

    view.editNice(3, TARGET_A);
    EXPECT_TRUE(view.error().empty());
}

TEST(ProcessPriorityViewTest, NullActionsGiveAnUnavailableError)
{
    ProcessPriorityView view;
    view.editNice(5, TARGET_A);
    view.apply(nullptr, TARGET_A, NICE_OF_A);
    EXPECT_EQ(view.error(), "Process actions unavailable");
    EXPECT_FALSE(view.hasPendingEdit());
}

// --- No replay, no other process -----------------------------------------------------------------

TEST(ProcessPriorityViewTest, ASecondApplyIsNotAReplay)
{
    for (const bool succeeds : {true, false})
    {
        SCOPED_TRACE(succeeds ? "after success" : "after failure");
        TestMocks::MockProcessActions mock;
        mock.setPriorityResult(succeeds ? Platform::ProcessActionResult::ok() : Platform::ProcessActionResult::error("denied"));
        ProcessPriorityView view;
        view.editNice(8, TARGET_A);
        view.apply(&mock, TARGET_A, NICE_OF_A);
        view.apply(&mock, TARGET_A, NICE_OF_A);
        EXPECT_EQ(mock.setPriorityCount(), 1);
        EXPECT_FALSE(view.canApply(NICE_OF_A, TARGET_A));
    }
}

TEST(ProcessPriorityViewTest, ApplyWithNothingPendingDoesNothing)
{
    TestMocks::MockProcessActions mock;
    mock.setPriorityResult(Platform::ProcessActionResult::error("denied"));
    ProcessPriorityView view;
    view.editNice(8, TARGET_A);
    view.apply(&mock, TARGET_A, NICE_OF_A);
    ASSERT_EQ(view.error(), "denied");

    view.apply(&mock, TARGET_A, NICE_OF_A);
    EXPECT_EQ(mock.setPriorityCount(), 1);
    EXPECT_EQ(view.error(), "denied"); // Left as it was
}

TEST(ProcessPriorityViewTest, ASelectionChangeResetsTheEditAndTheError)
{
    TestMocks::MockProcessActions mock;
    mock.setPriorityResult(Platform::ProcessActionResult::error("denied"));
    ProcessPriorityView view;
    view.editNice(-3, TARGET_A);
    view.apply(&mock, TARGET_A, NICE_OF_A);
    view.editNice(12, TARGET_A);
    ASSERT_TRUE(view.hasPendingEdit());

    view.onSelectionChanged();

    EXPECT_FALSE(view.hasPendingEdit());
    EXPECT_EQ(view.niceValue(), Domain::Priority::NORMAL_NICE);
    EXPECT_EQ(view.editTarget().pid, -1);
    EXPECT_TRUE(view.error().empty());
}

TEST(ProcessPriorityViewTest, AnEditIsNotAppliedAfterASelectionChange)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    view.editNice(12, TARGET_A);
    view.onSelectionChanged(); // B is selected
    view.apply(&mock, TARGET_B, 0);
    EXPECT_EQ(mock.setPriorityCount(), 0);
}

TEST(ProcessPriorityViewTest, AnEditIsNotAppliedToAnotherProcess)
{
    // The live target moved without a selection change: a different PID, or A's PID reused by a later
    // process. The edit is dropped, with no platform call.
    for (const Platform::ProcessTarget& other : {TARGET_B, TARGET_A_REUSED})
    {
        SCOPED_TRACE("pid=" + std::to_string(other.pid) + " start=" + std::to_string(other.startTimeTicks));
        TestMocks::MockProcessActions mock;
        ProcessPriorityView view;
        view.editNice(12, TARGET_A);
        view.apply(&mock, other, 0);
        EXPECT_EQ(mock.setPriorityCount(), 0);
        EXPECT_FALSE(view.hasPendingEdit());
        EXPECT_EQ(view.editTarget().pid, -1);
    }
}

TEST(ProcessPriorityViewTest, DropEditIfTargetMovedKeepsAnEditForTheSameProcess)
{
    ProcessPriorityView view;
    view.editNice(12, TARGET_A);
    EXPECT_FALSE(view.dropEditIfTargetMoved(TARGET_A));
    EXPECT_FALSE(view.dropEditIfTargetMoved(TARGET_A_UNCONFIRMED)); // Live start time unknown: not a move
    EXPECT_TRUE(view.hasPendingEdit());

    EXPECT_TRUE(view.dropEditIfTargetMoved(TARGET_B));
    EXPECT_FALSE(view.hasPendingEdit());
    EXPECT_FALSE(view.dropEditIfTargetMoved(TARGET_B)); // Nothing left to drop
}

TEST(ProcessPriorityViewTest, DropEditIfTargetMovedDropsAnUnknownIdentityOnceKnown)
{
    ProcessPriorityView view;
    view.editNice(12, TARGET_A_UNCONFIRMED);
    EXPECT_FALSE(view.dropEditIfTargetMoved(TARGET_A_UNCONFIRMED)); // Still unknown: kept
    EXPECT_TRUE(view.hasPendingEdit());

    EXPECT_TRUE(view.dropEditIfTargetMoved(TARGET_A)); // Now known: the user edits again
    EXPECT_FALSE(view.hasPendingEdit());
}

} // namespace
} // namespace App
