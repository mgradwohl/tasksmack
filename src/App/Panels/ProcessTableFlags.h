#pragma once

// Pure table-flag selection for the Processes table, extracted so the tree-view/list-view
// difference is unit-testable without a live ImGui context, following CONTRIBUTING.md's "extract
// the pure decision logic into a small header" pattern (as ProcessTreeFlatten.h and
// ProcessTreeIndent.h do).

#include <imgui.h>

namespace App::ProcessTableFlags
{

/// Flags shared by both view modes.
inline constexpr ImGuiTableFlags BASE = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY |
                                        ImGuiTableFlags_ScrollX | ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingFixedFit;

/// Table flags for the Processes table.
///
/// Sorting is only offered in list view. Tree view renders in natural (parent/child) order and
/// ignores the sort specs entirely, so advertising sortable headers there is an affordance with no
/// effect: they highlight on hover, accept the click, keep showing the sort arrow left over from
/// list view, and change nothing (#926). Dropping ImGuiTableFlags_Sortable removes the arrow and
/// the hover response, so the headers stop promising something the mode cannot do.
[[nodiscard]] constexpr ImGuiTableFlags forProcessTable(bool treeViewEnabled) noexcept
{
    return treeViewEnabled ? BASE : (BASE | ImGuiTableFlags_Sortable);
}

} // namespace App::ProcessTableFlags
