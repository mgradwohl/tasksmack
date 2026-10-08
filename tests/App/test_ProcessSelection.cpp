/// @file test_ProcessSelection.cpp
/// @brief The Processes table's multi-selection model (#804): plain, Ctrl and Shift clicks, Ctrl+A,
/// the range anchor, and selected processes dropping out when they exit -- always by exact identity,
/// PID and start time (#1503), so neither a reused PID nor a colliding uniqueKey hash is ever selected.

#include "App/Panels/ProcessSelection.h"
#include "App/Panels/ProcessTableNavigation.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace App::ProcessSelection
{
namespace
{

/// The process with PID @p pid and a start time derived from it: distinct PIDs, distinct identities.
constexpr Identity id(std::int32_t pid)
{
    return {.pid = pid, .startTimeTicks = static_cast<std::uint64_t>(pid) * 10U};
}

/// The visible rows, in drawn order.
constexpr std::array<Identity, 6> ROWS{id(101), id(102), id(103), id(104), id(105), id(106)};

/// Stands in for Domain::ProcessSnapshot: retainPresent() reads pid and startTimeTicks. uniqueKey is
/// carried so a test can force two processes to share one, and show it is never what is matched.
struct FakeSnapshot
{
    std::int32_t pid = 0;
    std::uint64_t startTimeTicks = 0;
    std::uint64_t uniqueKey = 0;
};

/// The snapshot of @p identity, with uniqueKey @p key.
constexpr FakeSnapshot snap(Identity identity, std::uint64_t key = 0)
{
    return {.pid = identity.pid, .startTimeTicks = identity.startTimeTicks, .uniqueKey = key};
}

TEST(ProcessSelectionTest, ModifiersPickTheClickKind)
{
    EXPECT_EQ(clickKindFor(false, false), ClickKind::Replace);
    EXPECT_EQ(clickKindFor(true, false), ClickKind::Toggle);
    EXPECT_EQ(clickKindFor(false, true), ClickKind::Range);
    EXPECT_EQ(clickKindFor(true, true), ClickKind::AddRange);
}

TEST(ProcessSelectionTest, IdentityComparesPidAndStartTime)
{
    EXPECT_EQ(id(101), (Identity{.pid = 101, .startTimeTicks = 1010}));
    EXPECT_NE(id(101), (Identity{.pid = 101, .startTimeTicks = 1011})); // A reused PID
    EXPECT_NE(id(101), (Identity{.pid = 102, .startTimeTicks = 1010}));
    EXPECT_EQ(identityOf(snap(id(7), 99)), id(7));
    EXPECT_EQ(IdentityHash{}(id(7)), IdentityHash{}(id(7)));
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
    sel.click(ClickKind::Replace, id(102), ROWS);
    sel.click(ClickKind::Toggle, id(104), ROWS);
    sel.click(ClickKind::Replace, id(105), ROWS);
    EXPECT_EQ(sel.size(), 1U);
    EXPECT_TRUE(sel.contains(id(105)));
    EXPECT_EQ(sel.anchor(), id(105));
}

TEST(ProcessSelectionTest, AReusedPidIsNotTheSelectedProcess)
{
    Selection sel;
    sel.click(ClickKind::Replace, id(102), ROWS);
    EXPECT_FALSE(sel.contains(Identity{.pid = 102, .startTimeTicks = 9999}));
}

TEST(ProcessSelectionTest, AnyVisibleIsFalseOnceAFilterHidesEverySelectedRow)
{
    Selection sel;
    sel.click(ClickKind::Replace, id(102), ROWS);
    sel.click(ClickKind::Toggle, id(104), ROWS);
    EXPECT_TRUE(sel.anyVisible(ROWS));

    // A filter now shows only rows that are not selected; the selection itself is kept.
    constexpr std::array<Identity, 2> FILTERED{id(101), id(105)};
    EXPECT_FALSE(sel.anyVisible(FILTERED));
    EXPECT_EQ(sel.size(), 2U);
    EXPECT_FALSE(sel.anyVisible(std::span<const Identity>{}));
}

TEST(ProcessSelectionTest, CtrlClickTogglesOneRowAndKeepsTheOthers)
{
    Selection sel;
    sel.click(ClickKind::Replace, id(101), ROWS);
    sel.click(ClickKind::Toggle, id(103), ROWS);
    EXPECT_EQ(sel.size(), 2U);
    EXPECT_TRUE(sel.contains(id(101)));
    EXPECT_TRUE(sel.contains(id(103)));
    EXPECT_EQ(sel.anchor(), id(103));

    EXPECT_FALSE(sel.toggle(id(101))); // Removed
    EXPECT_EQ(sel.size(), 1U);
    EXPECT_FALSE(sel.contains(id(101)));
    EXPECT_TRUE(sel.toggle(id(101))); // Added back
    EXPECT_TRUE(sel.contains(id(101)));
}

TEST(ProcessSelectionTest, ShiftClickSelectsTheRangeFromTheAnchorInEitherDirection)
{
    Selection sel;
    sel.click(ClickKind::Replace, id(102), ROWS);
    sel.click(ClickKind::Range, id(105), ROWS);
    EXPECT_EQ(sel.size(), 4U);
    for (const std::int32_t pid : {102, 103, 104, 105})
    {
        EXPECT_TRUE(sel.contains(id(pid))) << pid;
    }
    EXPECT_EQ(sel.anchor(), id(102)); // A range does not move the anchor

    // A second Shift+click re-draws the range from the same anchor, upwards this time.
    sel.click(ClickKind::Range, id(101), ROWS);
    EXPECT_EQ(sel.size(), 2U);
    EXPECT_TRUE(sel.contains(id(101)));
    EXPECT_TRUE(sel.contains(id(102)));
    EXPECT_FALSE(sel.contains(id(105)));
}

TEST(ProcessSelectionTest, ShiftClickFollowsTheVisibleOrderNotThePids)
{
    // Sorted by some column, the rows are drawn in an order unrelated to their PIDs.
    const std::vector<Identity> sorted{id(106), id(101), id(104), id(102), id(105), id(103)};
    Selection sel;
    sel.click(ClickKind::Replace, id(101), sorted);
    sel.click(ClickKind::Range, id(102), sorted);
    EXPECT_EQ(sel.size(), 3U);
    EXPECT_TRUE(sel.contains(id(101)));
    EXPECT_TRUE(sel.contains(id(104)));
    EXPECT_TRUE(sel.contains(id(102)));
    EXPECT_FALSE(sel.contains(id(103)));
}

TEST(ProcessSelectionTest, CtrlShiftClickAddsTheRange)
{
    Selection sel;
    sel.click(ClickKind::Replace, id(101), ROWS);
    sel.click(ClickKind::Toggle, id(104), ROWS); // Anchor now at 104
    sel.click(ClickKind::AddRange, id(106), ROWS);
    EXPECT_EQ(sel.size(), 4U);
    for (const std::int32_t pid : {101, 104, 105, 106})
    {
        EXPECT_TRUE(sel.contains(id(pid))) << pid;
    }
}

TEST(ProcessSelectionTest, ShiftClickWithoutAVisibleAnchorActsAsAPlainClick)
{
    Selection none;
    none.click(ClickKind::Range, id(103), ROWS);
    EXPECT_EQ(none.size(), 1U);
    EXPECT_TRUE(none.contains(id(103)));
    EXPECT_EQ(none.anchor(), id(103));

    // The anchor was filtered out (or collapsed away): not in the visible rows.
    Selection hidden;
    hidden.click(ClickKind::Replace, id(999), ROWS);
    hidden.click(ClickKind::Range, id(104), ROWS);
    EXPECT_EQ(hidden.size(), 1U);
    EXPECT_TRUE(hidden.contains(id(104)));

    // Ctrl+Shift with no anchor adds the row, like a Ctrl+click.
    Selection add;
    add.click(ClickKind::Replace, id(999), ROWS);
    add.click(ClickKind::AddRange, id(104), ROWS);
    EXPECT_EQ(add.size(), 2U);
    EXPECT_TRUE(add.contains(id(999)));
    EXPECT_TRUE(add.contains(id(104)));
}

TEST(ProcessSelectionTest, ARangeFromAnExitedAnchorDoesNotStartAtItsReusedPid)
{
    // The anchor's process exited and its PID now belongs to a visible newcomer: the range must not
    // start from the newcomer's row, so the Shift+click acts as a plain click.
    Selection sel;
    sel.click(ClickKind::Replace, Identity{.pid = 102, .startTimeTicks = 1}, ROWS);
    sel.click(ClickKind::Range, id(105), ROWS);
    EXPECT_EQ(sel.size(), 1U);
    EXPECT_TRUE(sel.contains(id(105)));
}

TEST(ProcessSelectionTest, SelectAllTakesEveryVisibleRow)
{
    Selection sel;
    sel.click(ClickKind::Replace, id(999), ROWS); // Not visible: replaced
    sel.selectAll(ROWS);
    EXPECT_EQ(sel.size(), ROWS.size());
    EXPECT_FALSE(sel.contains(id(999)));
    EXPECT_EQ(sel.anchor(), id(101)); // The old anchor is not among them: the first row

    Selection kept;
    kept.click(ClickKind::Replace, id(104), ROWS);
    kept.selectAll(ROWS);
    EXPECT_EQ(kept.anchor(), id(104));

    Selection empty;
    empty.selectAll(std::span<const Identity>{});
    EXPECT_TRUE(empty.empty());
    EXPECT_FALSE(empty.anchor().has_value());
}

TEST(ProcessSelectionTest, ExitedProcessesDropOut)
{
    Selection sel;
    sel.selectAll(ROWS);
    const std::vector<FakeSnapshot> unchanged{
        snap(id(101)), snap(id(102)), snap(id(103)), snap(id(104)), snap(id(105)), snap(id(106)), snap(id(200))};
    EXPECT_FALSE(sel.retainPresent(unchanged));
    EXPECT_EQ(sel.size(), ROWS.size());

    // 102 and 105 exited; 205 is a new process and PID 102 was handed to another, neither selected.
    const Identity reused{.pid = 102, .startTimeTicks = 5555};
    const std::vector<FakeSnapshot> later{snap(id(101)), snap(id(103)), snap(id(104)), snap(id(106)), snap(id(205)), snap(reused)};
    EXPECT_TRUE(sel.retainPresent(later));
    EXPECT_EQ(sel.size(), 4U);
    EXPECT_FALSE(sel.contains(id(102)));
    EXPECT_FALSE(sel.contains(id(105)));
    EXPECT_FALSE(sel.contains(id(205)));
    EXPECT_FALSE(sel.contains(reused));

    const std::vector<FakeSnapshot> gone;
    EXPECT_TRUE(sel.retainPresent(gone));
    EXPECT_TRUE(sel.empty());
    EXPECT_FALSE(sel.retainPresent(gone)); // Nothing left to drop
}

// ========== Forced uniqueKey collisions (#1503) ==========
//
// Two different processes carrying the same uniqueKey: the hash is never what the selection matches,
// so whichever way the collision arises, it cannot move the selection or hand it an action.

constexpr std::uint64_t COLLIDING_KEY = 0xC0111DEU;

TEST(ProcessSelectionTest, SelectionDoesNotMoveToACollidingProcessThatAppearsAfterTheSelectedOneExits)
{
    // The case #1486's mitigations could not cover: never alive at the same moment.
    const Identity selected{.pid = 10, .startTimeTicks = 1000};
    const Identity newcomer{.pid = 20, .startTimeTicks = 2000};
    Selection sel;
    sel.click(ClickKind::Replace, selected, std::span<const Identity>{});
    EXPECT_FALSE(sel.retainPresent(std::array{snap(selected, COLLIDING_KEY)}));

    // The selected process exits; the newcomer appears with the same key.
    EXPECT_TRUE(sel.retainPresent(std::array{snap(newcomer, COLLIDING_KEY)}));
    EXPECT_TRUE(sel.empty());
    EXPECT_FALSE(sel.contains(newcomer));
}

TEST(ProcessSelectionTest, SelectionDoesNotMoveToAReusedPidWhoseKeyAlsoCollides)
{
    const Identity selected{.pid = 10, .startTimeTicks = 1000};
    const Identity reused{.pid = 10, .startTimeTicks = 3000};
    Selection sel;
    sel.selectOnly(selected);
    EXPECT_TRUE(sel.retainPresent(std::array{snap(reused, COLLIDING_KEY)}));
    EXPECT_TRUE(sel.empty());
    EXPECT_FALSE(sel.contains(reused));
}

TEST(ProcessSelectionTest, ACollidingLiveProcessNeitherJoinsNorHidesTheSelection)
{
    // Both alive together, sharing a key. The selected one stays selected -- no tombstone is needed --
    // and the other is never selected. When the selected one exits, the selection empties rather than
    // passing to the survivor.
    const Identity selected{.pid = 10, .startTimeTicks = 1000};
    const Identity other{.pid = 20, .startTimeTicks = 2000};
    Selection sel;
    sel.selectOnly(selected);
    EXPECT_FALSE(sel.retainPresent(std::array{snap(selected, COLLIDING_KEY), snap(other, COLLIDING_KEY)}));
    EXPECT_EQ(sel.size(), 1U);
    EXPECT_TRUE(sel.contains(selected));
    EXPECT_FALSE(sel.contains(other));

    EXPECT_TRUE(sel.retainPresent(std::array{snap(other, COLLIDING_KEY)}));
    EXPECT_TRUE(sel.empty());
}

TEST(ProcessSelectionTest, AnExitIsSeenEvenWhenTwoLiveRowsShareASelectedKey)
{
    // 101 and 102 selected; 102 exits while 101 and a newcomer share 101's key. 102 drops out, 101 stays.
    Selection sel;
    sel.click(ClickKind::Replace, id(101), ROWS);
    sel.click(ClickKind::Toggle, id(102), ROWS);
    const std::array live{snap(id(101), COLLIDING_KEY), snap(id(300), COLLIDING_KEY), snap(id(103))};
    EXPECT_TRUE(sel.retainPresent(live));
    EXPECT_EQ(sel.size(), 1U);
    EXPECT_TRUE(sel.contains(id(101)));
    EXPECT_FALSE(sel.contains(id(102)));
    EXPECT_FALSE(sel.contains(id(300)));
}

TEST(ProcessSelectionTest, TheShortcutFindsOnlyTheSelectedRowAmongCollidingOnes)
{
    // F9 with one row selected looks that row up among the visible ones by identity
    // (ProcessesPanel::applyKeyboardInput()). Rows sharing its key, or its PID, are not it.
    const Identity selected{.pid = 10, .startTimeTicks = 1000};
    const std::vector<Identity> visible{Identity{.pid = 20, .startTimeTicks = 2000}, // Same key, other PID
                                        Identity{.pid = 10, .startTimeTicks = 3000}, // Same key, reused PID
                                        selected};
    Selection sel;
    sel.selectOnly(selected);
    const Identity highlighted = *sel.selected().begin();
    EXPECT_EQ(ProcessTableNavigation::indexOfKey<Identity>(visible, highlighted), std::optional<std::size_t>{2});

    // Once it has exited, nothing is found, so F9 does nothing.
    const std::vector<Identity> afterExit{visible[0], visible[1]};
    EXPECT_EQ(ProcessTableNavigation::indexOfKey<Identity>(afterExit, highlighted), std::nullopt);
}

TEST(ProcessSelectionTest, ClearForgetsTheAnchor)
{
    Selection sel;
    sel.click(ClickKind::Replace, id(101), ROWS);
    sel.clear();
    EXPECT_TRUE(sel.empty());
    EXPECT_FALSE(sel.anchor().has_value());
}

} // namespace
} // namespace App::ProcessSelection
