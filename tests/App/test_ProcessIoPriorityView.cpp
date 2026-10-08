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

#include <expected>
#include <optional>
#include <string>

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

// --- Row layout (the panel does not scroll horizontally) ------------------------------------------

constexpr float EM = 16.0F;
constexpr float GAP = 8.0F;
constexpr float APPLY = 180.0F; // 11.25 em, the Apply floor

TEST(ProcessIoPriorityViewTest, AWideRowKeepsTheAuthoredWidthsOnOneLine)
{
    const Detail::IoPriorityRowLayout layout = Detail::computeIoPriorityRowLayout(1000.0F, EM, GAP, APPLY, true);
    EXPECT_FLOAT_EQ(layout.comboWidth, Detail::IO_PRIORITY_CLASS_COMBO_WIDTH_EM * EM);
    EXPECT_FLOAT_EQ(layout.sliderWidth, Detail::IO_PRIORITY_LEVEL_SLIDER_WIDTH_EM * EM);
    EXPECT_FLOAT_EQ(layout.applyWidth, APPLY);
    EXPECT_FALSE(layout.applyOnNewLine);
    EXPECT_FALSE(layout.sliderOnNewLine);
}

TEST(ProcessIoPriorityViewTest, ANarrowRowShrinksTheControlsBesideApply)
{
    // 400 px: Apply reserved first, the combo and slider shrink proportionally into the remaining 212 px.
    const Detail::IoPriorityRowLayout layout = Detail::computeIoPriorityRowLayout(400.0F, EM, GAP, APPLY, true);
    EXPECT_FALSE(layout.applyOnNewLine);
    EXPECT_FALSE(layout.sliderOnNewLine);
    EXPECT_FLOAT_EQ(layout.applyWidth, APPLY);
    EXPECT_LT(layout.comboWidth, Detail::IO_PRIORITY_CLASS_COMBO_WIDTH_EM * EM);
    EXPECT_FLOAT_EQ(layout.comboWidth, layout.sliderWidth);
    EXPECT_GE(layout.comboWidth, Detail::IO_PRIORITY_CLASS_COMBO_WIDTH_EM * EM * Detail::IO_PRIORITY_MIN_WIDTH_FRACTION);
    EXPECT_LE(layout.comboWidth + GAP + layout.sliderWidth + GAP + layout.applyWidth, 400.0F + 0.001F);
}

TEST(ProcessIoPriorityViewTest, BelowTheMinimumApplyWrapsOntoItsOwnLine)
{
    const Detail::IoPriorityRowLayout layout = Detail::computeIoPriorityRowLayout(300.0F, EM, GAP, APPLY, true);
    EXPECT_TRUE(layout.applyOnNewLine);
    EXPECT_FALSE(layout.sliderOnNewLine);
    EXPECT_LE(layout.comboWidth + GAP + layout.sliderWidth, 300.0F + 0.001F);

    // Without a slider the combo alone needs less, but Apply still wraps once even that cannot fit.
    const Detail::IoPriorityRowLayout idle = Detail::computeIoPriorityRowLayout(250.0F, EM, GAP, APPLY, false);
    EXPECT_TRUE(idle.applyOnNewLine);
    EXPECT_FLOAT_EQ(idle.sliderWidth, 0.0F);
}

TEST(ProcessIoPriorityViewTest, AVeryNarrowRowStacksEverythingWithinThePanel)
{
    const Detail::IoPriorityRowLayout layout = Detail::computeIoPriorityRowLayout(100.0F, EM, GAP, APPLY, true);
    EXPECT_TRUE(layout.applyOnNewLine);
    EXPECT_TRUE(layout.sliderOnNewLine);
    EXPECT_FLOAT_EQ(layout.comboWidth, 100.0F);
    EXPECT_FLOAT_EQ(layout.sliderWidth, 100.0F);
    EXPECT_FLOAT_EQ(layout.applyWidth, 100.0F);
}

TEST(ProcessIoPriorityViewTest, NoLineIsEverWiderThanThePanel)
{
    for (const bool hasSlider : {true, false})
    {
        for (int pixels = 20; pixels <= 1200; pixels += 10)
        {
            SCOPED_TRACE(pixels);
            const auto width = static_cast<float>(pixels);
            const Detail::IoPriorityRowLayout layout = Detail::computeIoPriorityRowLayout(width, EM, GAP, APPLY, hasSlider);
            const float slider = (hasSlider && !layout.sliderOnNewLine) ? GAP + layout.sliderWidth : 0.0F;
            const float apply = layout.applyOnNewLine ? 0.0F : GAP + layout.applyWidth;
            EXPECT_LE(layout.comboWidth + slider + apply, width + 0.001F);
            EXPECT_LE(layout.sliderWidth, width + 0.001F);
            EXPECT_LE(layout.applyWidth, width + 0.001F);
        }
    }
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
