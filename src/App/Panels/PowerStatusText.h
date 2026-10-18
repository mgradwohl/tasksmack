#pragma once

// The Power section header's battery status text, extracted from SystemMetricsPanel so the
// state-to-text mapping is unit-testable without a live ImGui context.

#include "Domain/SystemSnapshot.h"
#include "UI/IconsFontAwesome6.h"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace App::Detail
{

/// Shown in place of a charge the battery didn't report (chargePercent < 0), never "-1%" (#2408)
inline constexpr std::string_view NO_CHARGE_TEXT = "â";

/// The charge as "NN%", or a dash when the battery didn't report one.
[[nodiscard]] inline std::string formatChargePercent(int chargePercent)
{
    return chargePercent < 0 ? std::string(NO_CHARGE_TEXT) : std::format("{}%", chargePercent);
}

/// The battery icon for a charge level.
[[nodiscard]] inline const char* batteryIcon(int chargePercent)
{
    if (chargePercent >= 87)
    {
        return ICON_FA_BATTERY_FULL;
    }
    if (chargePercent >= 62)
    {
        return ICON_FA_BATTERY_THREE_QUARTERS;
    }
    if (chargePercent >= 37)
    {
        return ICON_FA_BATTERY_HALF;
    }
    if (chargePercent >= 12)
    {
        return ICON_FA_BATTERY_QUARTER;
    }
    return ICON_FA_BATTERY_EMPTY;
}

/// " (h:mm <suffix>)", or empty for an unknown (0) duration.
[[nodiscard]] inline std::string formatBatteryDuration(std::uint64_t seconds, const char* suffix)
{
    if (seconds == 0)
    {
        return {};
    }
    return std::format(" ({:d}:{:02d} {})", seconds / 3600, (seconds % 3600) / 60, suffix);
}

/// The right-hand Power header text for a battery-equipped machine.
///
/// Whether the machine is on AC (the plug) comes from PowerStatus::isOnAc, which on Linux is the
/// adapter's own report, not from the battery state: a battery can report a stale Discharging
/// while plugged in, or Full while the adapter is offline (#1109). The battery state then says
/// what the battery itself is doing.
[[nodiscard]] inline std::string batteryHeaderStatus(const Domain::PowerStatus& power)
{
    const int charge = power.chargePercent;
    const std::string percent = formatChargePercent(charge);
    const std::string plug = power.isOnAc ? std::string(ICON_FA_PLUG) + " " : std::string();

    if (power.isCharging)
    {
        return std::format("{} {} {}{}", ICON_FA_BOLT, batteryIcon(charge), percent, formatBatteryDuration(power.timeToFullSec, "to full"));
    }
    if (power.isFull)
    {
        return std::format("{}{} 100%", plug, ICON_FA_BATTERY_FULL);
    }
    if (power.isNotCharging)
    {
        // Plugged in but held below full, often by a charge threshold: its real charge, not "100%" (#1158).
        return std::format("{}{} {} (not charging)", plug, batteryIcon(charge), percent);
    }
    if (power.isDischarging && !power.isOnAc)
    {
        return std::format("{} {}{}", batteryIcon(charge), percent, formatBatteryDuration(power.timeToEmptySec, "left"));
    }
    return std::format("{}{} {}", plug, batteryIcon(charge), percent);
}

} // namespace App::Detail
