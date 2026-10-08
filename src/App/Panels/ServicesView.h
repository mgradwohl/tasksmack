#pragma once

// The Services tab's table (#800, phase 1: read-only). Split from ServicesPanel, which owns the model
// and its sampler, so the drawing runs headless in TaskSmackTests without a Platform probe. The pure
// helpers (labels, colours, row order) make no ImGui calls.

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

/// The theme's status colour for a state: running green, pending amber, paused as stopped
/// processes, stopped and unknown muted.
[[nodiscard]] ImVec4 serviceStateColor(Platform::ServiceState state, const UI::ColorScheme& scheme) noexcept;

/// Indices into `services` of those whose name or display name contains `filter` (ASCII case-
/// insensitive; empty matches all), ordered by `column`. Ties keep the name order the model sorted.
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
    ServiceColumn sortColumn = ServiceColumn::Name;
    bool ascending = true;

    std::vector<std::size_t> rows;
    std::uint64_t rowsVersion = 0;
    std::string rowsFilter;
    ServiceColumn rowsColumn = ServiceColumn::Name;
    bool rowsAscending = true;

    std::string unavailableHeading; ///< The empty state's heading when the list can't be read.
};

/// Draws the filter box and the table (or an empty state) into the current window.
/// @param publication Null is treated as no sample yet.
ServicesViewContent renderServicesView(const Domain::ServicePublication* publication,
                                       const Platform::ServiceCapabilities& capabilities,
                                       ServicesViewState& state);

} // namespace App
