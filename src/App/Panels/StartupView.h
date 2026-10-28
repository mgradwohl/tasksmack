#pragma once

// The Startup tab's table (#801; enable / disable in phase 2). Split from StartupPanel, which owns the model
// and its sampler, so the drawing runs headless in TaskSmackTests without a Platform probe. The pure
// helpers (labels, row order) make no ImGui calls.

#include "Domain/StartupImpact.h"
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
    Impact, ///< Startup impact (#3073); hidden where the platform has no impact data
    Scope,
    Location,
    Command,
};

[[nodiscard]] std::string_view startupLocationLabel(Platform::StartupLocation location) noexcept;
[[nodiscard]] std::string_view startupScopeLabel(Platform::StartupScope scope) noexcept;

/// "Enabled", "Disabled since <local date>", or "Disabled" when no time is recorded; "Enabled by
/// policy" or "Disabled by policy" when a policy sets it (#1873).
[[nodiscard]] std::string startupEnabledLabel(const Platform::StartupEntry& entry);

/// The Startup impact column's text (#3073): "High", "Medium", "Low", "Not measured", or "None" for a
/// disabled entry, as Task Manager shows it.
[[nodiscard]] std::string_view startupImpactLabel(Domain::StartupImpact impact) noexcept;

/// The Startup impact cell's tooltip (#3073), one line per fact: for a measured entry, its CPU time and
/// disk I/O at the sign-in that began at @p signInUnixSeconds, how many processes they add up, and the
/// thresholds its rating comes from; for one not measured, why (@p unavailableReason when the
/// measurement couldn't be read); for a disabled one, that it doesn't start.
[[nodiscard]] std::string
startupImpactTooltip(const Domain::StartupEntryImpact& impact, std::uint64_t signInUnixSeconds, std::string_view unavailableReason);

/// The name that identifies @p entry within its location (#1873): the name it is registered under,
/// else (an entry from a probe that gives none) the name shown.
[[nodiscard]] std::string_view startupEntryKey(const Platform::StartupEntry& entry) noexcept;

/// The tooltip's line for the name @p entry is registered under (#1873): "Registry value:
/// MicrosoftEdgeAutoLaunch_..." for a Run or RunOnce entry whose value name differs from the name
/// shown, or "Startup task: <family>!<TaskId>" for a packaged app. Empty otherwise (a Startup folder
/// entry's tooltip shows its file's path already).
[[nodiscard]] std::string startupRegistrationLabel(const Platform::StartupEntry& entry);

/// The tooltip's line for the program's own name (#1971): "Program name: Windows Command Processor"
/// for an entry shown by another name (a RunOnce entry, shown by its registry value name). Empty when
/// the program's name is unknown or is the name shown.
[[nodiscard]] std::string startupProgramNameLabel(const Platform::StartupEntry& entry);

/// The file the entry's icon is read from (#1800): a packaged app's logo, else the program it starts;
/// empty -- the generic icon -- when that program wasn't found or couldn't be told.
[[nodiscard]] std::string_view startupIconPath(const Platform::StartupEntry& entry) noexcept;

/// Indices into `entries` of those whose name, registration name (#1873), publisher or command contains `filter` (ASCII case-
/// insensitive; empty matches all), ordered by `column`. By Enabled, the enabled entries come first
/// (last when descending) and each group is sorted A to Z by name (case-insensitively; the exact name
/// and then the location break ties) in either direction. Other columns' ties keep the name order
/// the model sorted. By Startup impact (#3073), from None through Not measured, Low and Medium to High,
/// and within a rating by CPU time, then disk I/O; @p impacts is by index into @p entries, and an entry
/// past its end counts as Not measured.
[[nodiscard]] std::vector<std::size_t> buildStartupRows(std::span<const Platform::StartupEntry> entries,
                                                        std::string_view filter,
                                                        StartupColumn column,
                                                        bool ascending,
                                                        std::span<const Domain::StartupEntryImpact> impacts = {});

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
    std::vector<std::uint64_t> iconKeys;    ///< Per entry: Domain::iconFileKey(startupIconPath(entry)) (#1800).
    std::uint64_t rowsVersion = 0;
    std::string rowsFilter;
    StartupColumn rowsColumn = StartupColumn::Enabled;
    bool rowsAscending = true;

    std::string unavailableHeading; ///< The empty state's heading when there is no startup list.

    /// The selected row (phase 2): its startupEntryKey, location and package family together, since one
    /// name can be registered in more than one place. Empty name when none.
    std::string selectedName;
    Platform::StartupLocation selectedLocation = Platform::StartupLocation::RunUser;
    std::string selectedFamily; ///< The selected packaged task's family (#1873); empty for other entries.
};

class StartupActionsView;
class ProcessIconView;

/// Draws the filter box and the table (or an empty state) into the current window, with @p actions'
/// action bar, result line and row menus when it is given (#801, phase 2), and each entry's icon
/// beside its name through @p icons when it is given (#1800; the visible rows only).
/// @param publication Null is treated as no sample yet.
StartupViewContent renderStartupView(const Domain::StartupPublication* publication,
                                     const Platform::StartupCapabilities& capabilities,
                                     StartupViewState& state,
                                     StartupActionsView* actions = nullptr,
                                     ProcessIconView* icons = nullptr);

} // namespace App
