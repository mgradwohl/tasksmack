#pragma once

// The Startup tab's table (#801; enable / disable in phase 2). Split from StartupPanel, which owns the model
// and its sampler, so the drawing runs headless in TaskSmackTests without a Platform probe. The pure
// helpers (labels, row order) make no ImGui calls.

#include "Domain/StartupModel.h"
#include "Platform/IStartupProbe.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace App
{

/// The table's columns, in display order; also the ImGui column user IDs the sort specs report.
enum class StartupColumn : std::uint8_t
{
    Name,
    Publisher,
    Enabled,
    Scope,
    Location,
    Command,
};

[[nodiscard]] std::string_view startupLocationLabel(Platform::StartupLocation location) noexcept;
[[nodiscard]] std::string_view startupScopeLabel(Platform::StartupScope scope) noexcept;

/// "Enabled", "Disabled since <local date>", or "Disabled" when no time is recorded.
[[nodiscard]] std::string startupEnabledLabel(const Platform::StartupEntry& entry);

/// Indices into `entries` of those whose name, publisher or command contains `filter` (ASCII case-
/// insensitive; empty matches all), ordered by `column`. By Enabled, the enabled entries come first
/// (last when descending) and each group is sorted A to Z by name (case-insensitively; the exact name
/// and then the location break ties) in either direction. Other columns' ties keep the name order
/// the model sorted.
[[nodiscard]] std::vector<std::size_t>
buildStartupRows(std::span<const Platform::StartupEntry> entries, std::string_view filter, StartupColumn column, bool ascending);

/// What the view drew this frame.
enum class StartupViewContent : std::uint8_t
{
    Unsupported, ///< The platform has no startup list.
    Loading,     ///< No sample yet.
    Table,
};

/// The view's state between frames: the filter text, sort order and what is built from them. The
/// rows (and the Enabled column's text) are rebuilt only when the publication, filter or sort
/// changes, never every frame (#580).
struct StartupViewState
{
    std::string filter;
    StartupColumn sortColumn = StartupColumn::Enabled; ///< The default (#1597): enabled entries first.
    bool ascending = true;

    std::vector<std::size_t> rows;
    std::vector<std::string> enabledLabels; ///< Per entry of the publication, not per row.
    std::uint64_t rowsVersion = 0;
    std::string rowsFilter;
    StartupColumn rowsColumn = StartupColumn::Enabled;
    bool rowsAscending = true;

    std::string unavailableHeading; ///< The empty state's heading when there is no startup list.

    /// The selected row (phase 2): its name and location together, since one name can be registered
    /// in more than one place. Empty name when none.
    std::string selectedName;
    Platform::StartupLocation selectedLocation = Platform::StartupLocation::RunUser;
};

class StartupActionsView;

/// Draws the filter box and the table (or an empty state) into the current window, with @p actions'
/// action bar, result line and row menus when it is given (#801, phase 2).
/// @param publication Null is treated as no sample yet.
StartupViewContent renderStartupView(const Domain::StartupPublication* publication,
                                     const Platform::StartupCapabilities& capabilities,
                                     StartupViewState& state,
                                     StartupActionsView* actions = nullptr);

} // namespace App
