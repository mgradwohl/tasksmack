/// @file test_ProcessSelection.cpp
/// @brief The Processes table's multi-selection model (#804): plain, Ctrl and Shift clicks, Ctrl+A,
/// the range anchor, and selected processes dropping out when they exit -- always by uniqueKey, so a
/// reused PID is never selected.

#include "App/Panels/ProcessSelection.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace App::ProcessSelection
{
namespace
{

/// The visible rows, in drawn order, by uniqueKey.
constexpr std::array<std::uint64_t, 6> ROWS{101, 102, 103, 104, 105, 106};

/// Stands in for Domain::ProcessSnapshot: retainPresent() reads only uniqueKey.
struct FakeSnapshot
{
    std::uint64_t uniqueKey = 0;
};

TEST(ProcessSelectionTest, ModifiersPickTheClickKind)
{
    EXPECT_EQ(clickKindFor(false, false), ClickKind::Replace);
    EXPECT_EQ(clickKindFor(true, false), ClickKind::Toggle);
    EXPECT_EQ(clickKindFor(false, true), ClickKind::Range);
    EXPECT_EQ(clickKindFor(true, true), ClickKind::AddRange);
}

TEST(ProcessSelectionTest, StartsEmpty)
{
    const Selection sel;
    EXPECT_TRUE(sel.empty());
    EXPECT_EQ(sel.size(), 0U);
    EXPECT_FALSE(sel.anchor().has_value());
}

TEST(ProcessSelectionTest, PlainClickSelectsOnlyThatRow)
{
    Selection sel;
    sel.click(ClickKind::Replace, 102, ROWS);
    sel.click(ClickKind::Toggle, 104, ROWS);
    sel.click(ClickKind::Replace, 105, ROWS);
    EXPECT_EQ(sel.size(), 1U);
    EXPECT_TRUE(sel.contains(105));
    EXPECT_EQ(sel.anchor(), 105U);
}

TEST(ProcessSelectionTest, AnyVisibleIsFalseOnceAFilterHidesEverySelectedRow)
{
    Selection sel;
    sel.click(ClickKind::Replace, 102, ROWS);
    sel.click(ClickKind::Toggle, 104, ROWS);
    EXPECT_TRUE(sel.anyVisible(ROWS));

    // A filter now shows only rows that are not selected; the selection itself is kept.
    constexpr std::array<std::uint64_t, 2> FILTERED{101, 105};
    EXPECT_FALSE(sel.anyVisible(FILTERED));
    EXPECT_EQ(sel.size(), 2U);
    EXPECT_FALSE(sel.anyVisible(std::span<const std::uint64_t>{}));
}

TEST(ProcessSelectionTest, CtrlClickTogglesOneRowAndKeepsTheOthers)
{
    Selection sel;
    sel.click(ClickKind::Replace, 101, ROWS);
    sel.click(ClickKind::Toggle, 103, ROWS);
    EXPECT_EQ(sel.size(), 2U);
    EXPECT_TRUE(sel.contains(101));
    EXPECT_TRUE(sel.contains(103));
    EXPECT_EQ(sel.anchor(), 103U);

    EXPECT_FALSE(sel.toggle(101)); // Removed
    EXPECT_EQ(sel.size(), 1U);
    EXPECT_FALSE(sel.contains(101));
    EXPECT_TRUE(sel.toggle(101)); // Added back
    EXPECT_TRUE(sel.contains(101));
}

TEST(ProcessSelectionTest, ShiftClickSelectsTheRangeFromTheAnchorInEitherDirection)
{
    Selection sel;
    sel.click(ClickKind::Replace, 102, ROWS);
    sel.click(ClickKind::Range, 105, ROWS);
    EXPECT_EQ(sel.size(), 4U);
    for (const std::uint64_t key : {102U, 103U, 104U, 105U})
    {
        EXPECT_TRUE(sel.contains(key)) << key;
    }
    EXPECT_EQ(sel.anchor(), 102U); // A range does not move the anchor

    // A second Shift+click re-draws the range from the same anchor, upwards this time.
    sel.click(ClickKind::Range, 101, ROWS);
    EXPECT_EQ(sel.size(), 2U);
    EXPECT_TRUE(sel.contains(101));
    EXPECT_TRUE(sel.contains(102));
    EXPECT_FALSE(sel.contains(105));
}

TEST(ProcessSelectionTest, ShiftClickFollowsTheVisibleOrderNotTheKeys)
{
    // Sorted by some column, the rows are drawn in an order unrelated to their keys.
    const std::vector<std::uint64_t> sorted{106, 101, 104, 102, 105, 103};
    Selection sel;
    sel.click(ClickKind::Replace, 101, sorted);
    sel.click(ClickKind::Range, 102, sorted);
    EXPECT_EQ(sel.size(), 3U);
    EXPECT_TRUE(sel.contains(101));
    EXPECT_TRUE(sel.contains(104));
    EXPECT_TRUE(sel.contains(102));
    EXPECT_FALSE(sel.contains(103));
}

TEST(ProcessSelectionTest, CtrlShiftClickAddsTheRange)
{
    Selection sel;
    sel.click(ClickKind::Replace, 101, ROWS);
    sel.click(ClickKind::Toggle, 104, ROWS); // Anchor now at 104
    sel.click(ClickKind::AddRange, 106, ROWS);
    EXPECT_EQ(sel.size(), 4U);
    for (const std::uint64_t key : {101U, 104U, 105U, 106U})
    {
        EXPECT_TRUE(sel.contains(key)) << key;
    }
}

TEST(ProcessSelectionTest, ShiftClickWithoutAVisibleAnchorActsAsAPlainClick)
{
    Selection none;
    none.click(ClickKind::Range, 103, ROWS);
    EXPECT_EQ(none.size(), 1U);
    EXPECT_TRUE(none.contains(103));
    EXPECT_EQ(none.anchor(), 103U);

    // The anchor was filtered out (or collapsed away): not in the visible rows.
    Selection hidden;
    hidden.click(ClickKind::Replace, 999, ROWS);
    hidden.click(ClickKind::Range, 104, ROWS);
    EXPECT_EQ(hidden.size(), 1U);
    EXPECT_TRUE(hidden.contains(104));

    // Ctrl+Shift with no anchor adds the row, like a Ctrl+click.
    Selection add;
    add.click(ClickKind::Replace, 999, ROWS);
    add.click(ClickKind::AddRange, 104, ROWS);
    EXPECT_EQ(add.size(), 2U);
    EXPECT_TRUE(add.contains(999));
    EXPECT_TRUE(add.contains(104));
}

TEST(ProcessSelectionTest, SelectAllTakesEveryVisibleRow)
{
    Selection sel;
    sel.click(ClickKind::Replace, 999, ROWS); // Not visible: replaced
    sel.selectAll(ROWS);
    EXPECT_EQ(sel.size(), ROWS.size());
    EXPECT_FALSE(sel.contains(999));
    EXPECT_EQ(sel.anchor(), 101U); // The old anchor is not among them: the first row

    Selection kept;
    kept.click(ClickKind::Replace, 104, ROWS);
    kept.selectAll(ROWS);
    EXPECT_EQ(kept.anchor(), 104U);

    Selection empty;
    empty.selectAll(std::span<const std::uint64_t>{});
    EXPECT_TRUE(empty.empty());
    EXPECT_FALSE(empty.anchor().has_value());
}

TEST(ProcessSelectionTest, AnExitIsSeenEvenWhenTwoLiveRowsShareASelectedKey)
{
    // 101 and 102 selected; 102 exits while two live rows carry key 101 (a hash collision). Counting
    // rows would see 2 == 2 and keep 102; counting distinct keys drops it.
    Selection sel;
    sel.click(ClickKind::Replace, 101, ROWS);
    sel.click(ClickKind::Toggle, 102, ROWS);
    const std::array<FakeSnapshot, 3> live{FakeSnapshot{.uniqueKey = 101}, FakeSnapshot{.uniqueKey = 101}, FakeSnapshot{.uniqueKey = 103}};
    EXPECT_TRUE(sel.retainPresent(live));
    EXPECT_EQ(sel.size(), 1U);
    EXPECT_TRUE(sel.contains(101));
    EXPECT_FALSE(sel.contains(102));
}

TEST(ProcessSelectionTest, AKeySeenOnTwoLiveRowsStaysRefusedAfterOneExits)
{
    // 101 selected; two live rows carry 101 (a hash collision), then the selected one exits. The
    // survivor still carries 101, but the key stays refused until the selection is replaced.
    Selection sel;
    sel.click(ClickKind::Replace, 101, ROWS);
    EXPECT_TRUE(sel.isActionable(101));
    const std::array<FakeSnapshot, 2> both{FakeSnapshot{.uniqueKey = 101}, FakeSnapshot{.uniqueKey = 101}};
    EXPECT_FALSE(sel.retainPresent(both));
    EXPECT_FALSE(sel.isActionable(101));
    const std::array<FakeSnapshot, 1> survivor{FakeSnapshot{.uniqueKey = 101}};
    EXPECT_FALSE(sel.retainPresent(survivor));
    EXPECT_TRUE(sel.contains(101));
    EXPECT_FALSE(sel.isActionable(101));
    // Replacing the selection clears the refusal.
    sel.click(ClickKind::Replace, 101, ROWS);
    EXPECT_TRUE(sel.isActionable(101));
}

TEST(ProcessSelectionTest, ExitedProcessesDropOut)
{
    Selection sel;
    sel.selectAll(ROWS);
    const std::vector<FakeSnapshot> unchanged{{101}, {102}, {103}, {104}, {105}, {106}, {200}};
    EXPECT_FALSE(sel.retainPresent(unchanged));
    EXPECT_EQ(sel.size(), ROWS.size());

    // 102 and 105 exited; 205 is a new process (it could hold a reused PID) and is not selected.
    const std::vector<FakeSnapshot> later{{101}, {103}, {104}, {106}, {205}};
    EXPECT_TRUE(sel.retainPresent(later));
    EXPECT_EQ(sel.size(), 4U);
    EXPECT_FALSE(sel.contains(102));
    EXPECT_FALSE(sel.contains(105));
    EXPECT_FALSE(sel.contains(205));

    const std::vector<FakeSnapshot> gone;
    EXPECT_TRUE(sel.retainPresent(gone));
    EXPECT_TRUE(sel.empty());
    EXPECT_FALSE(sel.retainPresent(gone)); // Nothing left to drop
}

TEST(ProcessSelectionTest, ClearForgetsTheAnchor)
{
    Selection sel;
    sel.click(ClickKind::Replace, 101, ROWS);
    sel.clear();
    EXPECT_TRUE(sel.empty());
    EXPECT_FALSE(sel.anchor().has_value());
}

} // namespace
} // namespace App::ProcessSelection
