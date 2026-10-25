#pragma once

// The Overview's battery details rows (#1523): design and full-charge capacity, wear, cycle count,
// chemistry, manufacturer and model. Pure (no ImGui calls), so unit-tested; CpuDetailsBlock::renderRows()
// draws them beneath the Battery chart. The serial number is an identifier and is never shown.

#include "App/Panels/CpuDetailsText.h"
#include "Domain/SystemSnapshot.h"
#include "UI/Format.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App::BatteryDetailsText
{

inline constexpr std::string_view NOT_REPORTED_BY_SYSTEM = "Not reported for this battery on this system.";
inline constexpr std::string_view NOT_REPORTED_BY_BATTERY = "The battery did not report it.";

/// Wear: the share of the design capacity the battery can no longer hold, 1 - full / design, as a
/// percentage. 0 when it holds its design capacity or more; nullopt unless both are known (> 0).
[[nodiscard]] inline std::optional<double> wearPercent(double fullChargeWh, double designWh) noexcept
{
    if (!(designWh > 0.0) || !(fullChargeWh > 0.0))
    {
        return std::nullopt;
    }
    return std::max(0.0, (1.0 - (fullChargeWh / designWh)) * 100.0);
}

/// A row: `value` when the platform reports this fact and the battery gave it (non-empty), else a
/// muted dash whose tooltip says which of the two is missing.
[[nodiscard]] inline CpuDetailsText::Row fact(std::string_view label, bool reported, std::string value)
{
    if (reported && !value.empty())
    {
        return {.label = label, .value = std::move(value), .available = true, .tooltip = {}};
    }
    return {.label = label,
            .value = std::string(CpuDetailsText::UNAVAILABLE_TEXT),
            .available = false,
            .tooltip = std::string(reported ? NOT_REPORTED_BY_BATTERY : NOT_REPORTED_BY_SYSTEM)};
}

/// The rows for a battery-equipped machine, in display order. Every row is always present, so the
/// block keeps its shape whatever the battery reports.
[[nodiscard]] inline std::vector<CpuDetailsText::Row> buildRows(const Domain::PowerStatus& power)
{
    const auto wattHours = [](double wh)
    {
        return wh > 0.0 ? UI::Format::formatFixedLocalized(UI::Format::roundHalfAwayFromZero(wh, 1), 1, " Wh") : std::string();
    };
    const std::optional<double> wear = wearPercent(power.fullChargeCapacityWh, power.designCapacityWh);

    std::vector<CpuDetailsText::Row> rows;
    rows.reserve(7);
    rows.push_back(fact("Design capacity", power.reportsCapacity, wattHours(power.designCapacityWh)));
    rows.push_back(fact("Full charge", power.reportsCapacity, wattHours(power.fullChargeCapacityWh)));
    rows.push_back(fact("Wear", power.reportsCapacity, wear ? UI::Format::formatPercent(*wear) : std::string()));
    rows.back().tooltip =
        wear ? "Share of the design capacity the battery can no longer hold." : "Needs both the design and the full-charge capacity.";
    rows.push_back(
        fact("Cycles", power.reportsCycleCount, power.cycleCount > 0 ? UI::Format::formatIntLocalized(power.cycleCount) : std::string()));
    rows.push_back(fact("Chemistry", power.reportsTechnology, power.technology));
    // No capability flag: the platform reports them whenever the battery does
    rows.push_back(fact("Manufacturer", true, power.manufacturer));
    rows.push_back(fact("Model", true, power.model));
    return rows;
}

} // namespace App::BatteryDetailsText
