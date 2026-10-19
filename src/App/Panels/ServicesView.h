#pragma once

// The Services tab's table (#800), with ServiceActionsView's bar and row menus. Split from
// ServicesPanel, which owns the model and its sampler, so the drawing runs headless in TaskSmackTests
// without a Platform probe. The pure helpers (labels, colours, row order) make no ImGui calls.

#include "Domain/ServiceModel.h"
#include "Platform/IServiceProbe.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace App
{

/// The table's columns, in display order; also the ImGui column user IDs the sort specs report.
enum class ServiceColumn : std::uint8_t
{
    Name,
    DisplayName,
    State,
    StartType,
    Pid,
    Account,
};

[[nodiscard]] std::string_view serviceStateLabel(Platform::ServiceState state) noexcept;
[[nodiscard]] std::string_view serviceStartTypeLabel(Platform::ServiceStartType startType) noexcept;

/// Where a state sorts in the State column (#1599), lowest first: running, then the states on the
/// way to or from running (starting, resuming, pausing), paused, stopping, stopped, and unknown last.
/// Ascending, a service that is up (or coming up) lists above one that is down (or going down).
[[nodiscard]] std::uint8_t serviceStateSortRank(Platform::ServiceState state) noexcept;

/// The theme's status colour for a state: running green, pending amber, paused as stopped
/// processes, stopped and unknown muted.
[[nodiscard]] ImVec4 serviceStateColor(Platform::ServiceState state, const UI::ColorScheme& scheme) noexcept;

/// Indices into `services` of those whose name or display name contains `filter` (ASCII case-
/// insensitive; empty matches all), ordered by `column`. By State, the states are grouped in
/// serviceStateSortRank() order (reversed when descending) and, within a state, sorted A to Z by
/// display name (case-insensitively; the service name breaks ties) in either direction. Other
/// columns' ties keep the name order the model sorted.
/// The file a service's icon is read from (#1800): its program, unless that is the shared service
/// host (svchost.exe, whose icon says nothing about the service) or couldn't be told. Empty: the
/// generic service icon.
[[nodiscard]] std::string_view serviceIconPath(const Platform::ServiceInfo& service) noexcept;

[[nodiscard]] std::vector<std::size_t>
buildServiceRows(std::span<const Platform::ServiceInfo> services, std::string_view filter, ServiceColumn column, bool ascending);

/// What the view drew this frame.
enum class ServicesViewContent : std::uint8_t
{
    Unsupported, ///< No service list: the platform has none, or the service manager refused.
    Loading,     ///< No sample yet.
    ReadFailed,  ///< The latest read failed and none has ever succeeded: the reason, no table.
    Table,
    StaleTable, ///< The table of the last good read, under a muted "out of date: <reason>" line.
};

/// The view's state between frames: the filter text, sort order and the rows built from them. The
/// rows are rebuilt only when the publication, filter or sort changes, never every frame (#580).
struct ServicesViewState
{
    std::string filter;
    ServiceColumn sortColumn = ServiceColumn::State; ///< The default (#1599): running services first.
    bool ascending = true;

    std::vector<std::size_t> rows;
    std::uint64_t rowsVersion = 0;
    /// Per service of the publication: Domain::iconFileKey(serviceIconPath(service)) (#1800), built
    /// once per sample.
    std::vector<std::uint64_t> iconKeys;
    std::uint64_t iconKeysVersion = 0;
    std::string rowsFilter;
    ServiceColumn rowsColumn = ServiceColumn::State;
    bool rowsAscending = true;

    std::string unavailableHeading; ///< The empty state's heading when the list can't be read.

    std::string selectedName;      ///< The selected row's service (#1577); empty when none.
    bool scrollToSelected = false; ///< Scroll the selected row into view on the next draw (#1886)
};

class ServiceActionsView;
class ProcessIconView;

/// Draws the filter box and the table (or an empty state) into the current window, with @p actions'
/// action bar, result line and row menus when it is given (#1577), and each service's icon beside its
/// name through @p icons when it is given (#1800; the visible rows only).
/// @param publication Null is treated as no sample yet.
ServicesViewContent renderServicesView(const Domain::ServicePublication* publication,
                                       const Platform::ServiceCapabilities& capabilities,
                                       ServicesViewState& state,
                                       ServiceActionsView* actions = nullptr,
                                       ProcessIconView* icons = nullptr);

} // namespace App
