#pragma once

// Pure width arithmetic for the Processes table, extracted so the fill-or-scroll decision is
// unit-testable without a live ImGui context, following CONTRIBUTING.md's "extract the pure decision
// logic into a small header" pattern (as ProcessTableFlags.h and ProcessTreeIndent.h do).

#include <cmath>

namespace App::ProcessTableLayout
{

/// Inner width to pass to ImGui::BeginTable() so the Command column fills the table without ever
/// collapsing (#924).
///
/// The table scrolls horizontally, and every column used to be fixed-width, so when the columns
/// summed to less than the window the remainder was simply left blank -- about half the window at
/// the Small preset. Command is the natural column to absorb that slack: it is unbounded text and
/// already last. But a plain stretch column cannot do it alone, because under ImGuiTableFlags_ScrollX
/// a stretch column is sized against the *visible* width: it fills when the other columns leave
/// room, and collapses to nothing as soon as they do not.
///
/// ImGui's answer is the inner_width parameter: an explicit content width that stretch columns are
/// sized against instead. So Command is a stretch column, and this picks the inner width:
///
///   - the other columns leave room for Command's minimum  -> 0, "fit the visible width", and
///     Command takes whatever is left with no scrollbar;
///   - they do not                                         -> exactly enough for the other columns
///     plus Command's minimum, so Command keeps a readable width and the table scrolls.
///
/// @param otherColumnsWidthPx   Width of everything in the table that is not Command's own content:
///                              the other enabled columns plus all cell padding and spacing.
///                              Measured from the previous frame's layout.
/// @param commandMinWidthPx     Narrowest Command may be before the table scrolls instead.
/// @param visibleWidthPx        Width of the table's visible area, from the previous frame.
/// @return 0 to let ImGui fit the visible width, otherwise the content width to scroll within.
[[nodiscard]] inline float computeInnerWidth(float otherColumnsWidthPx, float commandMinWidthPx, float visibleWidthPx) noexcept
{
    // No measurement yet (first frame), or a degenerate one: fit the visible width. The next frame
    // has real numbers and corrects it.
    if (!std::isfinite(otherColumnsWidthPx) || !std::isfinite(commandMinWidthPx) || !std::isfinite(visibleWidthPx) ||
        otherColumnsWidthPx <= 0.0F || visibleWidthPx <= 0.0F)
    {
        return 0.0F;
    }

    const float needed = otherColumnsWidthPx + (commandMinWidthPx > 0.0F ? commandMinWidthPx : 0.0F);
    return (needed > visibleWidthPx) ? needed : 0.0F;
}

} // namespace App::ProcessTableLayout
