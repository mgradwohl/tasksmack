/// @file test_ProcessIoPriorityView.cpp
/// @brief Tests for the I/O priority control's state (#803), without an ImGui context: the on-demand
/// read of the shown process's I/O priority, Apply's enablement, applying to a mock IProcessActions with
/// the edit's PID and start time, the error line, the reset on a selection change, and that an apply is
/// never replayed or sent to another process. render() is covered headless in
/// test_ProcessIoPriorityViewRender.cpp.

#include "App/Panels/ProcessIoPriorityView.h"
#include "App/Panels/ProcessPriorityView.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>

namespace App
{
namespace
{

using Platform::IoPriority;
using Platform::IoPriorityClass;

// Two different processes, and a later process given A's PID.
constexpr Platform::ProcessTarget TARGET_A{.pid = 1001, .startTimeTicks = 5000};
constexpr Platform::ProcessTarget TARGET_B{.pid = 2002, .startTimeTicks = 6000};
constexpr Platform::ProcessTarget TARGET_A_REUSED{.pid = 1001, .startTimeTicks = 9000};
// A before its first snapshot: the PID alone, start time not yet known.
constexpr Platform::ProcessTarget TARGET_A_UNCONFIRMED{.pid = 1001, .startTimeTicks = 0};

constexpr IoPriority BEST_EFFORT_4{.ioClass = IoPriorityClass::BestEffort, .level = 4};
constexpr IoPriority IDLE{.ioClass = IoPriorityClass::Idle, .level = 0};

/// A view that has read @p current for @p target, as render() does first.
void readCurrent(ProcessIoPriorityView& view,
                 TestMocks::MockProcessActions& mock,
                 const Platform::ProcessTarget& target,
                 IoPriority current)
{
    mock.setIoPriorityReadResult(current);
    (void) view.refreshCurrent(&mock, target, 0.0);
    view.syncToProcess();
}

// --- Helpers -------------------------------------------------------------------------------------

TEST(ProcessIoPriorityViewTest, ClassNamesAndLevels)
{
    EXPECT_EQ(Detail::ioPriorityClassName(IoPriorityClass::None), "Default");
    EXPECT_EQ(Detail::ioPriorityClassName(IoPriorityClass::BestEffort), "Best-effort");
    EXPECT_EQ(Detail::ioPriorityClassName(IoPriorityClass::Idle), "Idle");
    EXPECT_EQ(Detail::ioPriorityClassName(IoPriorityClass::Realtime), "Realtime");
    EXPECT_TRUE(Detail::ioClassHasLevels(IoPriorityClass::BestEffort));
    EXPECT_TRUE(Detail::ioClassHasLevels(IoPriorityClass::Realtime));
    EXPECT_FALSE(Detail::ioClassHasLevels(IoPriorityClass::Idle));
    EXPECT_FALSE(Detail::ioClassHasLevels(IoPriorityClass::None));
}

TEST(ProcessIoPriorityViewTest, NormalizeHoldsTheLevel)
{
    EXPECT_EQ(Detail::normalizeIoPriority({.ioClass = IoPriorityClass::BestEffort, .level = 12}).level, 7);
    EXPECT_EQ(Detail::normalizeIoPriority({.ioClass = IoPriorityClass::Realtime, .level = -1}).level, 0);
    EXPECT_EQ(Detail::normalizeIoPriority({.ioClass = IoPriorityClass::Idle, .level = 5}).level, 0);
    EXPECT_EQ(Detail::normalizeIoPriority({.ioClass = IoPriorityClass::None, .level = 3}).level, 0);
}

TEST(ProcessIoPriorityViewTest, DescribeUsesIoniceWords)
{
    EXPECT_EQ(Detail::describeIoPriority(BEST_EFFORT_4, 0), "best-effort 4");
    EXPECT_EQ(Detail::describeIoPriority(IDLE, 0), "idle");
    EXPECT_EQ(Detail::describeIoPriority({.ioClass = IoPriorityClass::Realtime, .level = 2}, 0), "realtime 2");
    EXPECT_EQ(Detail::describeIoPriority({}, 0), "default (best-effort 4 from nice)");
    EXPECT_EQ(Detail::describeIoPriority({}, 19), "default (best-effort 7 from nice)");
    EXPECT_EQ(Detail::describeIoPriority({}, -20), "default (best-effort 0 from nice)");
    EXPECT_EQ(Detail::describeIoPriority({}, std::nullopt), "default (from nice)");
}

// ========== The I/O priority slider's scale (#1540) ==========

TEST(ProcessIoPriorityViewTest, EveryStopRoundTripsThroughItsClassAndLevel)
{
    for (const bool realtime : {true, false})
    {
        const auto count = static_cast<std::int32_t>(realtime ? Detail::IO_PRIORITY_SLIDER.stops.size()
                                                              : Detail::IO_PRIORITY_SLIDER_NO_REALTIME.stops.size());
        EXPECT_EQ(count, realtime ? 17 : 9);
        for (std::int32_t i = 0; i < count; ++i)
        {
            const Platform::IoPriority priority = Detail::ioPriorityForStop(i, realtime);
            const Detail::IoStop stop = Detail::ioStopFor(priority, 0, realtime);
            EXPECT_EQ(stop.index, i) << "realtime " << realtime;
            EXPECT_FALSE(stop.inherited);
            // The stop's own name is what the badge shows for that class and level.
            const auto& stops = realtime ? Detail::IO_PRIORITY_SLIDER.stops : Detail::IO_PRIORITY_SLIDER_NO_REALTIME.stops;
            EXPECT_EQ(stops[static_cast<std::size_t>(i)].name, Detail::describeIoPriority(priority, std::nullopt));
        }
    }
}

TEST(ProcessIoPriorityViewTest, TheScaleRunsFromRealtimeZeroToIdle)
{
    EXPECT_EQ(Detail::ioPriorityForStop(0, true).ioClass, Platform::IoPriorityClass::Realtime);
    EXPECT_EQ(Detail::ioPriorityForStop(0, true).level, 0);
    EXPECT_EQ(Detail::ioPriorityForStop(8, true).ioClass, Platform::IoPriorityClass::BestEffort);
    EXPECT_EQ(Detail::ioPriorityForStop(16, true).ioClass, Platform::IoPriorityClass::Idle);
    // Without Realtime the slider starts at Best-effort 0.
    EXPECT_EQ(Detail::ioPriorityForStop(0, false).ioClass, Platform::IoPriorityClass::BestEffort);
    EXPECT_EQ(Detail::ioPriorityForStop(8, false).ioClass, Platform::IoPriorityClass::Idle);
    // Held to the scale.
    EXPECT_EQ(Detail::ioPriorityForStop(99, false).ioClass, Platform::IoPriorityClass::Idle);
    EXPECT_EQ(Detail::ioPriorityForStop(-3, true).level, 0);
    // The bands begin at Best-effort and Idle.
    EXPECT_EQ(Detail::IO_PRIORITY_SLIDER.bandStarts.size(), 2U);
    EXPECT_EQ(Detail::IO_PRIORITY_SLIDER.bandStarts[0], 8);
    EXPECT_EQ(Detail::IO_PRIORITY_SLIDER.bandStarts[1], 16);
    EXPECT_EQ(Detail::IO_PRIORITY_SLIDER_NO_REALTIME.bandStarts[0], 8);
}

TEST(ProcessIoPriorityViewTest, TheDefaultSitsAtTheNiceDerivedLevelInherited)
{
    const Platform::IoPriority none{.ioClass = Platform::IoPriorityClass::None, .level = 0};
    // (nice + 20) / 5: nice -20 is level 0, 0 is level 4, 19 is level 7.
    for (const auto& [nice, level] : {std::pair{-20, 0}, std::pair{0, 4}, std::pair{19, 7}})
    {
        const Detail::IoStop withRealtime = Detail::ioStopFor(none, nice, true);
        EXPECT_EQ(withRealtime.index, Detail::IO_LEVEL_COUNT + level) << "nice " << nice;
        EXPECT_TRUE(withRealtime.inherited);
        const Detail::IoStop without = Detail::ioStopFor(none, nice, false);
        EXPECT_EQ(without.index, level) << "nice " << nice;
        EXPECT_TRUE(without.inherited);
    }
}

TEST(ProcessIoPriorityViewTest, RealtimeWithoutThePrivilegeIsShownBeyondTheStart)
{
    const Platform::IoPriority realtime{.ioClass = Platform::IoPriorityClass::Realtime, .level = 3};
    EXPECT_EQ(Detail::ioStopFor(realtime, 0, false).index, Detail::PRIORITY_STOP_BEYOND_START);
    EXPECT_NE(Detail::IO_PRIORITY_SLIDER_NO_REALTIME.beyondStart, nullptr);
    EXPECT_EQ(Detail::IO_PRIORITY_SLIDER.beyondStart, nullptr); // With the privilege Realtime is on the track
    EXPECT_EQ(Detail::ioStopFor(realtime, 0, true).index, 3);
}

// --- The on-demand read --------------------------------------------------------------------------

TEST(ProcessIoPriorityViewTest, ReadsTheShownProcessOnceUntilDue)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(IDLE);
    ProcessIoPriorityView view;

    EXPECT_TRUE(view.refreshCurrent(&mock, TARGET_A, 10.0));
    EXPECT_EQ(mock.getIoPriorityCount(), 1);
    EXPECT_EQ(mock.lastIoPriorityReadTarget().pid, TARGET_A.pid);
    EXPECT_EQ(mock.lastIoPriorityReadTarget().startTimeTicks, TARGET_A.startTimeTicks);
    EXPECT_EQ(view.currentIoPriority(), std::optional<IoPriority>{IDLE});

    // Every frame in between reuses it; once the refresh interval has passed it is read again.
    EXPECT_FALSE(view.refreshCurrent(&mock, TARGET_A, 10.5));
    EXPECT_EQ(mock.getIoPriorityCount(), 1);
    EXPECT_TRUE(view.refreshCurrent(&mock, TARGET_A, 10.0 + Detail::IO_PRIORITY_REFRESH_SECONDS));
    EXPECT_EQ(mock.getIoPriorityCount(), 2);
}

TEST(ProcessIoPriorityViewTest, AnotherTargetIsReadAtOnce)
{
    TestMocks::MockProcessActions mock;
    ProcessIoPriorityView view;
    (void) view.refreshCurrent(&mock, TARGET_A, 0.0);
    EXPECT_TRUE(view.refreshCurrent(&mock, TARGET_A_REUSED, 0.1));
    EXPECT_EQ(mock.getIoPriorityCount(), 2);
    EXPECT_EQ(mock.lastIoPriorityReadTarget().startTimeTicks, TARGET_A_REUSED.startTimeTicks);
}

TEST(ProcessIoPriorityViewTest, AnUnknownStartTimeIsNotRead)
{
    TestMocks::MockProcessActions mock;
    ProcessIoPriorityView view;
    (void) view.refreshCurrent(&mock, TARGET_A_UNCONFIRMED, 0.0);
    EXPECT_EQ(mock.getIoPriorityCount(), 0);
    EXPECT_FALSE(view.currentIoPriority().has_value());
}

TEST(ProcessIoPriorityViewTest, AFailedReadKeepsItsMessageAndNoValue)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(std::unexpected(std::string("Process not found - may have already exited")));
    ProcessIoPriorityView view;
    (void) view.refreshCurrent(&mock, TARGET_A, 0.0);
    EXPECT_FALSE(view.currentIoPriority().has_value());
    EXPECT_EQ(view.readError(), "Process not found - may have already exited");
}

TEST(ProcessIoPriorityViewTest, TheControlFollowsTheProcessUntilEdited)
{
    TestMocks::MockProcessActions mock;
    ProcessIoPriorityView view;
    readCurrent(view, mock, TARGET_A, IDLE);
    EXPECT_EQ(view.shownIoPriority(), IDLE);
    EXPECT_FALSE(view.hasPendingEdit());

    view.editIoPriority(BEST_EFFORT_4, TARGET_A);
    mock.setIoPriorityReadResult(IoPriority{.ioClass = IoPriorityClass::BestEffort, .level = 1});
    (void) view.refreshCurrent(&mock, TARGET_A, 100.0);
    view.syncToProcess();
    EXPECT_EQ(view.shownIoPriority(), BEST_EFFORT_4); // The edit stays
}

TEST(ProcessIoPriorityViewTest, AnotherProcessWhoseReadFailsShowsNoLeftoverValues)
{
    // Edited for A; the pane now shows B, and B's read fails. Neither A's edit nor A's own value may
    // stay on screen under "current: unknown".
    TestMocks::MockProcessActions mock;
    ProcessIoPriorityView view;
    readCurrent(view, mock, TARGET_A, BEST_EFFORT_4);
    view.editIoPriority(IDLE, TARGET_A);

    mock.setIoPriorityReadResult(std::unexpected(std::string("unreadable")));
    EXPECT_TRUE(view.dropEditIfTargetMoved(TARGET_B)); // As render() does: drop, read, sync
    (void) view.refreshCurrent(&mock, TARGET_B, 0.1);
    view.syncToProcess();
    EXPECT_FALSE(view.hasPendingEdit());
    EXPECT_FALSE(view.currentIoPriority().has_value());
    EXPECT_EQ(view.shownIoPriority(), IoPriority{});

    // Without an edit too: A's read value is not carried over to B.
    ProcessIoPriorityView unedited;
    readCurrent(unedited, mock, TARGET_A, IDLE);
    mock.setIoPriorityReadResult(std::unexpected(std::string("unreadable")));
    (void) unedited.refreshCurrent(&mock, TARGET_B, 0.1);
    unedited.syncToProcess();
    EXPECT_EQ(unedited.shownIoPriority(), IoPriority{});
}

// --- Editing -------------------------------------------------------------------------------------

TEST(ProcessIoPriorityViewTest, PickingTheShownValueIsNoEdit)
{
    TestMocks::MockProcessActions mock;
    ProcessIoPriorityView view;
    readCurrent(view, mock, TARGET_A, BEST_EFFORT_4);
    view.editIoPriority(BEST_EFFORT_4, TARGET_A);
    EXPECT_FALSE(view.hasPendingEdit());
}

TEST(ProcessIoPriorityViewTest, AClassWithLevelsStartsAtTheNiceDerivedLevel)
{
    TestMocks::MockProcessActions mock;
    ProcessIoPriorityView view;
    readCurrent(view, mock, TARGET_A, {}); // Never set: follows nice

    view.editClass(IoPriorityClass::BestEffort, 10, TARGET_A);
    EXPECT_EQ(view.shownIoPriority(), (IoPriority{.ioClass = IoPriorityClass::BestEffort, .level = 6}));

    // Best-effort to Realtime keeps the level; to Idle drops it.
    view.editClass(IoPriorityClass::Realtime, 10, TARGET_A);
    EXPECT_EQ(view.shownIoPriority(), (IoPriority{.ioClass = IoPriorityClass::Realtime, .level = 6}));
    view.editClass(IoPriorityClass::Idle, 10, TARGET_A);
    EXPECT_EQ(view.shownIoPriority(), IDLE);
    EXPECT_TRUE(view.hasPendingEdit());
    EXPECT_EQ(view.editTarget().pid, TARGET_A.pid);
}

// --- Apply's enablement --------------------------------------------------------------------------

TEST(ProcessIoPriorityViewTest, ApplyIsDisabledUntilAnEdit)
{
    TestMocks::MockProcessActions mock;
    ProcessIoPriorityView view;
    readCurrent(view, mock, TARGET_A, BEST_EFFORT_4);
    EXPECT_FALSE(view.canApply(TARGET_A));
    view.editIoPriority(IDLE, TARGET_A);
    EXPECT_TRUE(view.canApply(TARGET_A));
}

TEST(ProcessIoPriorityViewTest, ApplyIsDisabledUntilTheCurrentValueIsRead)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(std::unexpected(std::string("unreadable")));
    ProcessIoPriorityView view;
    (void) view.refreshCurrent(&mock, TARGET_A, 0.0);
    view.editIoPriority(IDLE, TARGET_A);
    EXPECT_FALSE(view.canApply(TARGET_A));
    EXPECT_TRUE(view.waitingForProcessDetails(TARGET_A));

    view.apply(&mock, TARGET_A);
    EXPECT_EQ(mock.setIoPriorityCount(), 0);
}

TEST(ProcessIoPriorityViewTest, ApplyIsDisabledAndSendsNothingWhileTheStartTimeIsUnknown)
{
    TestMocks::MockProcessActions mock;
    ProcessIoPriorityView view;
    (void) view.refreshCurrent(&mock, TARGET_A_UNCONFIRMED, 0.0);
    view.editIoPriority(IDLE, TARGET_A_UNCONFIRMED);
    EXPECT_FALSE(view.canApply(TARGET_A_UNCONFIRMED));
    view.apply(&mock, TARGET_A_UNCONFIRMED);
    EXPECT_EQ(mock.setIoPriorityCount(), 0);
    EXPECT_TRUE(view.hasPendingEdit()); // Kept, but not applicable

    // Once the start time is known, the edit made without it is dropped rather than carried over.
    EXPECT_TRUE(view.dropEditIfTargetMoved(TARGET_A));
    EXPECT_FALSE(view.hasPendingEdit());
}

TEST(ProcessIoPriorityViewTest, AStartTimeMismatchIsRefused)
{
    // Edited for A; the PID now names a later process. canApply() is false without a drop first, and
    // apply() drops the edit and sends nothing.
    TestMocks::MockProcessActions mock;
    ProcessIoPriorityView view;
    readCurrent(view, mock, TARGET_A, BEST_EFFORT_4);
    view.editIoPriority(IDLE, TARGET_A);
    (void) view.refreshCurrent(&mock, TARGET_A_REUSED, 0.1);

    EXPECT_FALSE(view.canApply(TARGET_A_REUSED));
    view.apply(&mock, TARGET_A_REUSED);
    EXPECT_EQ(mock.setIoPriorityCount(), 0);
    EXPECT_FALSE(view.hasPendingEdit());
}

TEST(ProcessIoPriorityViewTest, AnEditIsNotAppliedToAnotherProcess)
{
    TestMocks::MockProcessActions mock;
    ProcessIoPriorityView view;
    readCurrent(view, mock, TARGET_A, BEST_EFFORT_4);
    view.editIoPriority(IDLE, TARGET_A);
    (void) view.refreshCurrent(&mock, TARGET_B, 0.1);

    EXPECT_FALSE(view.canApply(TARGET_B));
    view.apply(&mock, TARGET_B);
    EXPECT_EQ(mock.setIoPriorityCount(), 0);
}

// --- Apply ---------------------------------------------------------------------------------------

TEST(ProcessIoPriorityViewTest, ApplySetsTheEditOnTheTargetWithItsStartTime)
{
    TestMocks::MockProcessActions mock;
    ProcessIoPriorityView view;
    readCurrent(view, mock, TARGET_A, {});
    view.editIoPriority({.ioClass = IoPriorityClass::BestEffort, .level = 6}, TARGET_A);

    view.apply(&mock, TARGET_A);
    ASSERT_EQ(mock.setIoPriorityCount(), 1);
    EXPECT_EQ(mock.lastTarget().pid, TARGET_A.pid);
    EXPECT_EQ(mock.lastTarget().startTimeTicks, TARGET_A.startTimeTicks);
    EXPECT_EQ(mock.lastSetIoPriority(), (IoPriority{.ioClass = IoPriorityClass::BestEffort, .level = 6}));
    EXPECT_FALSE(view.hasPendingEdit());
    EXPECT_TRUE(view.error().empty());
}

TEST(ProcessIoPriorityViewTest, ASecondApplyIsNotAReplay)
{
    TestMocks::MockProcessActions mock;
    ProcessIoPriorityView view;
    readCurrent(view, mock, TARGET_A, BEST_EFFORT_4);
    view.editIoPriority(IDLE, TARGET_A);
    view.apply(&mock, TARGET_A);
    view.apply(&mock, TARGET_A);
    EXPECT_EQ(mock.setIoPriorityCount(), 1);
}

TEST(ProcessIoPriorityViewTest, ApplyAsksForAFreshRead)
{
    TestMocks::MockProcessActions mock;
    ProcessIoPriorityView view;
    readCurrent(view, mock, TARGET_A, BEST_EFFORT_4);
    view.editIoPriority(IDLE, TARGET_A);
    view.apply(&mock, TARGET_A);

    // Well inside the refresh interval, the next frame still reads what the process has now.
    mock.setIoPriorityReadResult(IDLE);
    EXPECT_TRUE(view.refreshCurrent(&mock, TARGET_A, 0.01));
    view.syncToProcess();
    EXPECT_EQ(view.shownIoPriority(), IDLE);
}

TEST(ProcessIoPriorityViewTest, FailureShowsThePlatformMessageAndRevertsTheControl)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityResult(
        Platform::ProcessActionResult::error("Permission denied: the Realtime I/O class needs CAP_SYS_NICE (or root)."));
    ProcessIoPriorityView view;
    readCurrent(view, mock, TARGET_A, BEST_EFFORT_4);
    view.editIoPriority({.ioClass = IoPriorityClass::Realtime, .level = 0}, TARGET_A);

    view.apply(&mock, TARGET_A);
    EXPECT_EQ(mock.setIoPriorityCount(), 1);
    EXPECT_EQ(view.error(), "Permission denied: the Realtime I/O class needs CAP_SYS_NICE (or root).");
    EXPECT_EQ(view.shownIoPriority(), BEST_EFFORT_4);

    // Another edit clears the error.
    view.editIoPriority(IDLE, TARGET_A);
    EXPECT_TRUE(view.error().empty());
}

TEST(ProcessIoPriorityViewTest, NullActionsGiveAnUnavailableError)
{
    TestMocks::MockProcessActions mock;
    ProcessIoPriorityView view;
    readCurrent(view, mock, TARGET_A, BEST_EFFORT_4);
    view.editIoPriority(IDLE, TARGET_A);
    view.apply(nullptr, TARGET_A);
    EXPECT_EQ(view.error(), "Process actions unavailable");
}

// --- Selection change ----------------------------------------------------------------------------

TEST(ProcessIoPriorityViewTest, ASelectionChangeResetsTheEditTheErrorAndTheRead)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityResult(Platform::ProcessActionResult::error("nope"));
    ProcessIoPriorityView view;
    readCurrent(view, mock, TARGET_A, BEST_EFFORT_4);
    view.editIoPriority(IDLE, TARGET_A);
    view.apply(&mock, TARGET_A);
    view.editIoPriority(IDLE, TARGET_A);
    ASSERT_TRUE(view.hasPendingEdit());

    view.onSelectionChanged();
    EXPECT_FALSE(view.hasPendingEdit());
    EXPECT_TRUE(view.error().empty());
    EXPECT_FALSE(view.currentIoPriority().has_value());
    EXPECT_EQ(view.editTarget().pid, -1);

    // Even back on A, the edit is gone: nothing to apply.
    readCurrent(view, mock, TARGET_A, BEST_EFFORT_4);
    view.apply(&mock, TARGET_A);
    EXPECT_EQ(mock.setIoPriorityCount(), 1);
}

TEST(ProcessIoPriorityViewTest, ThePriorityViewResetsItsIoControlOnASelectionChange)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView priorityView;
    readCurrent(priorityView.ioPriorityView(), mock, TARGET_A, BEST_EFFORT_4);
    priorityView.ioPriorityView().editIoPriority(IDLE, TARGET_A);
    priorityView.onSelectionChanged();
    EXPECT_FALSE(priorityView.ioPriorityView().hasPendingEdit());
}

} // namespace
} // namespace App
