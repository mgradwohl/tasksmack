#pragma once

// Keyboard navigation of the Processes table (#160): which key is which move, how far a move goes,
// and what Left/Right do in tree view. ImGui-free and unit-tested (test_ProcessTableNavigation.cpp).
// The rows are always the table's visible order -- the sorted list in list view, the flattened
// expanded tree in tree view -- after the filter, so a move follows exactly what is on screen.
//
// ProcessesPanel reads the keys through App/KeyboardInput.cpp, maps them with commandFor(), finds the
// selection in the visible order with indexOfKey(), and moves it with stepSelection() or treeStep().

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace App::ProcessTableNavigation
{

/// The keys the table listens to. J, K and G are the vim-style letters.
enum class NavKey : std::uint8_t
{
    Up,
    Down,
    PageUp,
    PageDown,
    Home,
    End,
    Left,
    Right,
    J,
    K,
    G,
};

/// A move of the selection.
enum class NavCommand : std::uint8_t
{
    None,
    Up,       ///< One row up (Up, k)
    Down,     ///< One row down (Down, j)
    PageUp,   ///< One page up
    PageDown, ///< One page down
    First,    ///< The first row (Home, g)
    Last,     ///< The last row (End, G)
    Left,     ///< Tree view: collapse, or go to the parent
    Right,    ///< Tree view: expand, or go to the first child
};

/// The modifiers held with a navigation key.
struct NavModifiers
{
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool super = false;
};

/// What @p key does with @p mods held. Ctrl, Alt or Super make it another chord (a held Ctrl is the
/// pane's freeze gesture, #928), so nothing moves. Shift only turns g into G; Shift with any other key
/// does nothing, which leaves Shift+arrows free for a range selection later.
[[nodiscard]] constexpr NavCommand commandFor(NavKey key, NavModifiers mods) noexcept
{
    if (mods.ctrl || mods.alt || mods.super)
    {
        return NavCommand::None;
    }
    if (key == NavKey::G)
    {
        return mods.shift ? NavCommand::Last : NavCommand::First;
    }
    if (mods.shift)
    {
        return NavCommand::None;
    }
    switch (key)
    {
    case NavKey::Up:
    case NavKey::K:
        return NavCommand::Up;
    case NavKey::Down:
    case NavKey::J:
        return NavCommand::Down;
    case NavKey::PageUp:
        return NavCommand::PageUp;
    case NavKey::PageDown:
        return NavCommand::PageDown;
    case NavKey::Home:
        return NavCommand::First;
    case NavKey::End:
        return NavCommand::Last;
    case NavKey::Left:
        return NavCommand::Left;
    case NavKey::Right:
        return NavCommand::Right;
    case NavKey::G:
        break; // Handled above
    }
    return NavCommand::None;
}

/// How many rows Page Up/Down move: the rows that fit in @p viewHeight (the table's scrolling area
/// below its frozen header) at @p rowHeight each, less one, so the row at the edge stays in view as
/// context. At least 1, also for a zero, negative or non-finite height.
[[nodiscard]] inline std::size_t pageStep(float viewHeight, float rowHeight) noexcept
{
    if (!std::isfinite(viewHeight) || !std::isfinite(rowHeight) || rowHeight <= 0.0F || viewHeight <= rowHeight)
    {
        return 1;
    }
    const auto visibleRows = static_cast<std::size_t>(std::floor(viewHeight / rowHeight));
    return std::max<std::size_t>(1, visibleRows - 1);
}

/// The position of @p key in @p visibleKeys (the uniqueKeys of the visible rows, in order), or none
/// when that process is not visible: unselected, filtered out, or under a collapsed parent.
[[nodiscard]] constexpr std::optional<std::size_t> indexOfKey(std::span<const std::uint64_t> visibleKeys, std::uint64_t key) noexcept
{
    const auto it = std::ranges::find(visibleKeys, key);
    if (it == visibleKeys.end())
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - visibleKeys.begin());
}

/// The row a vertical move lands on, among @p rowCount visible rows, from @p current (none when the
/// selection is not visible). Clamped at both ends; a page is @p page rows. With no visible selection,
/// every move selects the first row, except Last, which selects the last. None, Left and Right are not
/// vertical: they keep @p current. No rows: none.
[[nodiscard]] constexpr std::optional<std::size_t>
stepSelection(std::optional<std::size_t> current, NavCommand command, std::size_t rowCount, std::size_t page) noexcept
{
    if (rowCount == 0)
    {
        return std::nullopt;
    }
    const std::size_t last = rowCount - 1;
    const std::size_t step = std::max<std::size_t>(1, page);
    if (!current.has_value() || *current > last)
    {
        switch (command)
        {
        case NavCommand::Up:
        case NavCommand::Down:
        case NavCommand::PageUp:
        case NavCommand::PageDown:
        case NavCommand::First:
            return 0;
        case NavCommand::Last:
            return last;
        case NavCommand::None:
        case NavCommand::Left:
        case NavCommand::Right:
            return std::nullopt;
        }
        return std::nullopt;
    }
    const std::size_t at = *current;
    switch (command)
    {
    case NavCommand::Up:
        return (at > 0) ? at - 1 : 0;
    case NavCommand::Down:
        return std::min(at + 1, last);
    case NavCommand::PageUp:
        return (at > step) ? at - step : 0;
    case NavCommand::PageDown:
        return (last - at > step) ? at + step : last;
    case NavCommand::First:
        return 0;
    case NavCommand::Last:
        return last;
    case NavCommand::None:
    case NavCommand::Left:
    case NavCommand::Right:
        break;
    }
    return at;
}

/// What Left/Right needs to know about a visible tree row (ProcessTreeFlatten::ProcessTreeRow's shape).
struct TreeRowShape
{
    int depth = 0;
    bool hasChildren = false;
    bool isExpanded = false;
};

/// What a Left/Right press does in tree view.
enum class TreeStepKind : std::uint8_t
{
    None,     ///< Nothing to do (a root leaf with Left, a leaf with Right, no selection)
    Collapse, ///< Collapse the selected row
    Expand,   ///< Expand the selected row
    Select,   ///< Move the selection to row `index`
};

struct TreeStep
{
    TreeStepKind kind = TreeStepKind::None;
    std::size_t index = 0; ///< The row acted on: the selected row, or the row to select
};

/// Left/Right on row @p index of @p rows (the visible tree, in order), as in a file manager's tree:
/// Left collapses an expanded parent, otherwise moves to the row's parent; Right expands a collapsed
/// parent, otherwise moves to an expanded parent's first child. A row's parent is the nearest row
/// above it that is one level shallower.
[[nodiscard]] constexpr TreeStep treeStep(std::span<const TreeRowShape> rows, std::size_t index, NavCommand command) noexcept
{
    if (index >= rows.size() || (command != NavCommand::Left && command != NavCommand::Right))
    {
        return {};
    }
    const TreeRowShape& row = rows[index];
    if (command == NavCommand::Left)
    {
        if (row.hasChildren && row.isExpanded)
        {
            return {.kind = TreeStepKind::Collapse, .index = index};
        }
        for (std::size_t i = index; i > 0; --i)
        {
            if (rows[i - 1].depth < row.depth)
            {
                return {.kind = TreeStepKind::Select, .index = i - 1};
            }
        }
        return {};
    }
    if (!row.hasChildren)
    {
        return {};
    }
    if (!row.isExpanded)
    {
        return {.kind = TreeStepKind::Expand, .index = index};
    }
    // Expanded: its first child is the next row, one level deeper (the filter can leave none).
    if (index + 1 < rows.size() && rows[index + 1].depth > row.depth)
    {
        return {.kind = TreeStepKind::Select, .index = index + 1};
    }
    return {};
}

} // namespace App::ProcessTableNavigation
