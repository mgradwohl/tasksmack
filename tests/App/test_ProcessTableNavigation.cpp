/// @file test_ProcessTableNavigation.cpp
/// @brief The Processes table's keyboard navigation math (#160): key to move, page size, stepping and
/// clamping, finding the selection in the visible order, and tree view's Left/Right.

#include "App/Panels/ProcessTableNavigation.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace App::ProcessTableNavigation
{
namespace
{

TEST(ProcessTableNavigationTest, KeysMapToMoves)
{
    const NavModifiers none{};
    EXPECT_EQ(commandFor(NavKey::Up, none), NavCommand::Up);
    EXPECT_EQ(commandFor(NavKey::K, none), NavCommand::Up);
    EXPECT_EQ(commandFor(NavKey::Down, none), NavCommand::Down);
    EXPECT_EQ(commandFor(NavKey::J, none), NavCommand::Down);
    EXPECT_EQ(commandFor(NavKey::PageUp, none), NavCommand::PageUp);
    EXPECT_EQ(commandFor(NavKey::PageDown, none), NavCommand::PageDown);
    EXPECT_EQ(commandFor(NavKey::Home, none), NavCommand::First);
    EXPECT_EQ(commandFor(NavKey::G, none), NavCommand::First);
    EXPECT_EQ(commandFor(NavKey::End, none), NavCommand::Last);
    EXPECT_EQ(commandFor(NavKey::G, {.shift = true}), NavCommand::Last);
    EXPECT_EQ(commandFor(NavKey::Left, none), NavCommand::Left);
    EXPECT_EQ(commandFor(NavKey::Right, none), NavCommand::Right);
}

TEST(ProcessTableNavigationTest, ModifiersBlockMoves)
{
    constexpr std::array<NavKey, 11> ALL_KEYS{NavKey::Up,
                                              NavKey::Down,
                                              NavKey::PageUp,
                                              NavKey::PageDown,
                                              NavKey::Home,
                                              NavKey::End,
                                              NavKey::Left,
                                              NavKey::Right,
                                              NavKey::J,
                                              NavKey::K,
                                              NavKey::G};
    const std::array<NavModifiers, 3> blocking{{{.ctrl = true}, {.alt = true}, {.super = true}}};
    for (const NavModifiers& mods : blocking)
    {
        for (const NavKey key : ALL_KEYS)
        {
            SCOPED_TRACE("key " + std::to_string(static_cast<int>(key)));
            EXPECT_EQ(commandFor(key, mods), NavCommand::None);
        }
    }
    // Shift only makes g into G: Shift+J, Shift+arrows and the rest do nothing.
    for (const NavKey key : ALL_KEYS)
    {
        if (key != NavKey::G)
        {
            SCOPED_TRACE("shift+key " + std::to_string(static_cast<int>(key)));
            EXPECT_EQ(commandFor(key, {.shift = true}), NavCommand::None);
        }
    }
}

TEST(ProcessTableNavigationTest, PageStepKeepsOneRowOfContext)
{
    EXPECT_EQ(pageStep(200.0F, 20.0F), 9U); // 10 rows fit: move 9
    EXPECT_EQ(pageStep(219.0F, 20.0F), 9U); // A partial row does not count
    EXPECT_EQ(pageStep(40.0F, 20.0F), 1U);  // Two rows: move one
    EXPECT_EQ(pageStep(20.0F, 20.0F), 1U);  // One row
    EXPECT_EQ(pageStep(5.0F, 20.0F), 1U);   // Less than a row
    EXPECT_EQ(pageStep(0.0F, 20.0F), 1U);   // Not laid out yet
    EXPECT_EQ(pageStep(-10.0F, 20.0F), 1U);
    EXPECT_EQ(pageStep(200.0F, 0.0F), 1U);
    EXPECT_EQ(pageStep(std::numeric_limits<float>::quiet_NaN(), 20.0F), 1U);
    EXPECT_EQ(pageStep(std::numeric_limits<float>::infinity(), 20.0F), 1U);
}

TEST(ProcessTableNavigationTest, FindsTheSelectionInTheVisibleOrder)
{
    const std::vector<std::uint64_t> keys{30, 10, 20};
    EXPECT_EQ(indexOfKey<std::uint64_t>(keys, 30), std::optional<std::size_t>{0});
    EXPECT_EQ(indexOfKey<std::uint64_t>(keys, 20), std::optional<std::size_t>{2});
    EXPECT_EQ(indexOfKey<std::uint64_t>(keys, 99), std::nullopt); // Filtered out or collapsed away
    EXPECT_EQ(indexOfKey<std::uint64_t>({}, 10), std::nullopt);
}

TEST(ProcessTableNavigationTest, StepsAndClamps)
{
    constexpr std::size_t ROWS = 10;
    constexpr std::size_t PAGE = 4;
    EXPECT_EQ(stepSelection(3, NavCommand::Down, ROWS, PAGE), 4U);
    EXPECT_EQ(stepSelection(3, NavCommand::Up, ROWS, PAGE), 2U);
    EXPECT_EQ(stepSelection(9, NavCommand::Down, ROWS, PAGE), 9U); // Clamped at the end
    EXPECT_EQ(stepSelection(0, NavCommand::Up, ROWS, PAGE), 0U);   // and at the start
    EXPECT_EQ(stepSelection(3, NavCommand::PageDown, ROWS, PAGE), 7U);
    EXPECT_EQ(stepSelection(7, NavCommand::PageDown, ROWS, PAGE), 9U);
    EXPECT_EQ(stepSelection(7, NavCommand::PageUp, ROWS, PAGE), 3U);
    EXPECT_EQ(stepSelection(3, NavCommand::PageUp, ROWS, PAGE), 0U);
    EXPECT_EQ(stepSelection(4, NavCommand::PageUp, ROWS, PAGE), 0U);
    EXPECT_EQ(stepSelection(5, NavCommand::First, ROWS, PAGE), 0U);
    EXPECT_EQ(stepSelection(5, NavCommand::Last, ROWS, PAGE), 9U);
    EXPECT_EQ(stepSelection(5, NavCommand::None, ROWS, PAGE), 5U);
    EXPECT_EQ(stepSelection(5, NavCommand::Left, ROWS, PAGE), 5U);  // Not a vertical move
    EXPECT_EQ(stepSelection(3, NavCommand::PageDown, ROWS, 0), 4U); // A zero page still moves
}

TEST(ProcessTableNavigationTest, WithoutAVisibleSelectionMovesStartAtAnEnd)
{
    constexpr std::array<NavCommand, 5> TO_FIRST{
        NavCommand::Up, NavCommand::Down, NavCommand::PageUp, NavCommand::PageDown, NavCommand::First};
    for (const NavCommand command : TO_FIRST)
    {
        SCOPED_TRACE("command " + std::to_string(static_cast<int>(command)));
        EXPECT_EQ(stepSelection(std::nullopt, command, 5, 2), 0U);
        EXPECT_EQ(stepSelection(7, command, 5, 2), 0U); // A stale index past the end counts as none
    }
    EXPECT_EQ(stepSelection(std::nullopt, NavCommand::Last, 5, 2), 4U);
    EXPECT_EQ(stepSelection(std::nullopt, NavCommand::Left, 5, 2), std::nullopt);
    EXPECT_EQ(stepSelection(std::nullopt, NavCommand::None, 5, 2), std::nullopt);
}

TEST(ProcessTableNavigationTest, NoRowsNoMove)
{
    EXPECT_EQ(stepSelection(std::nullopt, NavCommand::Down, 0, 4), std::nullopt);
    EXPECT_EQ(stepSelection(0, NavCommand::Last, 0, 4), std::nullopt);
}

// A small visible tree, in drawn order:
//   0 init           (expanded)
//   1   sshd         (expanded)
//   2     bash       (leaf)
//   3   cron         (collapsed)
//   4 kthreadd       (leaf root)
constexpr std::array<TreeRowShape, 5> TREE{{
    {.depth = 0, .hasChildren = true, .isExpanded = true},
    {.depth = 1, .hasChildren = true, .isExpanded = true},
    {.depth = 2, .hasChildren = false, .isExpanded = false},
    {.depth = 1, .hasChildren = true, .isExpanded = false},
    {.depth = 0, .hasChildren = false, .isExpanded = false},
}};

TEST(ProcessTableNavigationTest, LeftCollapsesThenGoesToTheParent)
{
    EXPECT_EQ(treeStep(TREE, 1, NavCommand::Left).kind, TreeStepKind::Collapse);
    EXPECT_EQ(treeStep(TREE, 1, NavCommand::Left).index, 1U);

    const TreeStep fromLeaf = treeStep(TREE, 2, NavCommand::Left);
    EXPECT_EQ(fromLeaf.kind, TreeStepKind::Select);
    EXPECT_EQ(fromLeaf.index, 1U);

    const TreeStep fromCollapsed = treeStep(TREE, 3, NavCommand::Left); // Its parent, not sshd above it
    EXPECT_EQ(fromCollapsed.kind, TreeStepKind::Select);
    EXPECT_EQ(fromCollapsed.index, 0U);

    EXPECT_EQ(treeStep(TREE, 4, NavCommand::Left).kind, TreeStepKind::None); // A root leaf
}

TEST(ProcessTableNavigationTest, RightExpandsThenGoesToTheFirstChild)
{
    const TreeStep expand = treeStep(TREE, 3, NavCommand::Right);
    EXPECT_EQ(expand.kind, TreeStepKind::Expand);
    EXPECT_EQ(expand.index, 3U);

    const TreeStep toChild = treeStep(TREE, 0, NavCommand::Right);
    EXPECT_EQ(toChild.kind, TreeStepKind::Select);
    EXPECT_EQ(toChild.index, 1U);

    EXPECT_EQ(treeStep(TREE, 2, NavCommand::Right).kind, TreeStepKind::None); // A leaf
    EXPECT_EQ(treeStep(TREE, 4, NavCommand::Right).kind, TreeStepKind::None);
}

TEST(ProcessTableNavigationTest, LeftRightWithoutAVisibleSelectionSelectTheFirstRow)
{
    // Unselected, filtered out or collapsed away: Left and Right select the first row, as every other
    // move does (Copilot review on #1471).
    for (const NavCommand command : {NavCommand::Left, NavCommand::Right})
    {
        SCOPED_TRACE("command " + std::to_string(static_cast<int>(command)));
        const TreeStep none = treeStepFrom(TREE, std::nullopt, command);
        EXPECT_EQ(none.kind, TreeStepKind::Select);
        EXPECT_EQ(none.index, 0U);
        const TreeStep stale = treeStepFrom(TREE, 99, command); // Past the end counts as none
        EXPECT_EQ(stale.kind, TreeStepKind::Select);
        EXPECT_EQ(stale.index, 0U);
        EXPECT_EQ(treeStepFrom({}, std::nullopt, command).kind, TreeStepKind::None); // No rows
    }
    // A visible selection gets treeStep()'s answer; other moves are not Left/Right's business.
    EXPECT_EQ(treeStepFrom(TREE, 1, NavCommand::Left).kind, TreeStepKind::Collapse);
    EXPECT_EQ(treeStepFrom(TREE, 3, NavCommand::Right).kind, TreeStepKind::Expand);
    EXPECT_EQ(treeStepFrom(TREE, std::nullopt, NavCommand::Down).kind, TreeStepKind::None);
}

TEST(ProcessTableNavigationTest, TreeStepIgnoresOtherMovesAndBadRows)
{
    EXPECT_EQ(treeStep(TREE, 1, NavCommand::Down).kind, TreeStepKind::None);
    EXPECT_EQ(treeStep(TREE, 99, NavCommand::Left).kind, TreeStepKind::None);
    EXPECT_EQ(treeStep({}, 0, NavCommand::Right).kind, TreeStepKind::None);
    // An expanded parent whose children are all filtered out has no first child to go to.
    constexpr std::array<TreeRowShape, 2> bare{{{.depth = 0, .hasChildren = true, .isExpanded = true}, {.depth = 0}}};
    EXPECT_EQ(treeStep(bare, 0, NavCommand::Right).kind, TreeStepKind::None);
}

} // namespace
} // namespace App::ProcessTableNavigation
