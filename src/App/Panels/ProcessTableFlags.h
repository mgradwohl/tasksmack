#pragma once

// Pure table-flag selection for the Processes table, extracted so the flags are unit-testable
// without a live ImGui context, following CONTRIBUTING.md's "extract the pure decision logic into a
// small header" pattern (as ProcessTreeFlatten.h and ProcessTreeIndent.h do).

#include <imgui.h>

namespace App::ProcessTableFlags
{

/// Flags shared by both view modes.
inline constexpr ImGuiTableFlags BASE = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY |
                                        ImGuiTableFlags_ScrollX | ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingFixedFit;

/// Table flags for the Processes table, the same in both views.
///
/// Tree view was not sortable until #838: it drew rows in natural parent/child order and ignored the
/// sort specs, so its headers dropped ImGuiTableFlags_Sortable rather than promise a sort they could
/// not do (#926). Now it sorts each parent's children by the table's sort, with every child still
/// under its parent (ProcessTreeFlatten::buildProcessTreeRows()), so both views offer it.
[[nodiscard]] constexpr ImGuiTableFlags forProcessTable() noexcept
{
    return BASE | ImGuiTableFlags_Sortable;
}

} // namespace App::ProcessTableFlags
