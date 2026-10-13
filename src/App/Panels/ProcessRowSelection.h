#pragma once

// Which selected row of the Processes table is the primary one (#1926): the process Process Details
// shows, the Details tab is named after and single-process actions use. With several rows selected
// (#804) every one is drawn in the selection fill, so the primary row also gets a marker -- a bar at
// its left edge and an outline, in the theme's selectionPrimary -- and the Details tab adds the PID to
// the name, since the table often lists several processes of one name.
//
// ImGui-free decisions and geometry, unit-tested in test_ProcessRowSelection.cpp; ProcessesPanel draws
// the marker and ProcessDetailsPanel names its tab with them.

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

namespace App::ProcessRowSelection
{

/// How a row is drawn for the selection.
enum class RowState : std::uint8_t
{
    None,     ///< Not selected: no fill.
    Selected, ///< Selected: the selection fill.
    Primary,  ///< The primary row of a multi-selection: the selection fill and the marker.
};

/// How a row is drawn: @p selected says it is in the selection, @p primary that it is the process
/// Process Details shows, and @p selectionCount how many rows are selected. A lone selected row needs
/// no marker -- it is the only candidate -- so it is drawn as before multi-selection; a primary
/// process that is not in the selection (Ctrl+click took it out) is not marked either.
[[nodiscard]] constexpr RowState rowState(bool selected, bool primary, std::size_t selectionCount) noexcept
{
    if (!selected)
    {
        return RowState::None;
    }
    return (primary && selectionCount >= 2) ? RowState::Primary : RowState::Selected;
}

/// The marker's bar is this many ems wide (about 4 px at 18 px text), never under MIN_BAR_WIDTH_PX.
inline constexpr float MARKER_BAR_WIDTH_EM = 0.2F;
inline constexpr float MIN_BAR_WIDTH_PX = 2.0F;
/// The outline round the row: one pixel per OUTLINE_EM_PER_PX ems of text, never under one pixel.
inline constexpr float OUTLINE_EM_PER_PX = 16.0F;

/// Where the marker of a primary row is drawn.
struct Marker
{
    ImVec2 barMin;
    ImVec2 barMax;
    ImVec2 outlineMin;
    ImVec2 outlineMax;
    float outlineThickness = 1.0F;
};

/// The marker of the row whose selection fill is @p rowMin..@p rowMax, at @p fontSize px text. The
/// fill spans every column, including any scrolled out of view, so both parts are held to the visible
/// span @p visibleMinX..@p visibleMaxX: the bar stays at the left edge the user sees, and the outline's
/// sides stay in view. Whole pixels, so the bar's edge is crisp.
[[nodiscard]] inline Marker
markerFor(const ImVec2& rowMin, const ImVec2& rowMax, float visibleMinX, float visibleMaxX, float fontSize) noexcept
{
    const float left = std::max(rowMin.x, visibleMinX);
    const float right = std::max(left, std::min(rowMax.x, visibleMaxX));
    const float barWidth = std::max(MIN_BAR_WIDTH_PX, std::round(fontSize * MARKER_BAR_WIDTH_EM));
    return Marker{
        .barMin = ImVec2(left, rowMin.y),
        .barMax = ImVec2(std::min(left + barWidth, right), rowMax.y),
        .outlineMin = ImVec2(left, rowMin.y),
        .outlineMax = ImVec2(right, rowMax.y),
        .outlineThickness = std::max(1.0F, std::round(fontSize / OUTLINE_EM_PER_PX)),
    };
}

/// The Details tab's text into @p out (its capacity reused, so a steady label allocates nothing): the
/// process's name, and with several processes selected its PID too, "name (PID n)" as the row menu's
/// header puts it -- the name alone does not say which of several same-named processes is shown.
inline void formatDetailsTabText(std::string& out, std::string_view name, std::int32_t pid, std::size_t selectionCount)
{
    out.assign(name);
    if (selectionCount >= 2)
    {
        std::format_to(std::back_inserter(out), " (PID {})", pid);
    }
}

} // namespace App::ProcessRowSelection
