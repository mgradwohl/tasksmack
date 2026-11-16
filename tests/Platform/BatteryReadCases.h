#pragma once

// One table of battery readings both power probes must turn into the same PowerCounters (#4054): the live rate, the
// remaining charge and the voltage, with their signs, units and capabilities. Each platform's test writes a case into
// its own fake (Linux: sysfs files; Windows: IOCTL_BATTERY_QUERY_STATUS replies) and checks the probe's reading
// against the expected values here, so a sign or unit drift on one platform fails its test.
//
// Several batteries (not in this one-battery table; each platform tests it beside the table): the rates and the
// remaining Wh are summed, and the voltage is the first battery's that reports one, by the platform's own order.

#include "Platform/PowerTypes.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace TestSupport::BatteryRead
{

/// One battery as its driver reports it, and what the probe must make of it.
struct Case
{
    std::string_view name;
    Platform::BatteryState state = Platform::BatteryState::Unknown;
    // What the battery reports. A value the battery doesn't report is left out of the fake altogether.
    bool reportsRate = true;
    double rateW = 0.0; ///< The rate's size, in W; the driver's own sign is not relied on
    bool reportsCharge = true;
    double remainingWh = 0.0;
    double fullWh = 0.0;
    bool reportsVoltage = true;
    std::uint64_t voltageMv = 0;
    // What the probe must read.
    double expectedPowerNowW = 0.0; ///< Positive while discharging, negative while charging
    double expectedChargeNowWh = 0.0;
    std::uint64_t expectedVoltageMv = 0;
};

using Platform::BatteryState;

inline constexpr std::array CASES{
    Case{.name = "discharging",
         .state = BatteryState::Discharging,
         .rateW = 12.5,
         .remainingWh = 40.0,
         .fullWh = 50.0,
         .voltageMv = 11500,
         .expectedPowerNowW = 12.5,
         .expectedChargeNowWh = 40.0,
         .expectedVoltageMv = 11500},
    Case{.name = "charging",
         .state = BatteryState::Charging,
         .rateW = 20.0,
         .remainingWh = 30.0,
         .fullWh = 50.0,
         .voltageMv = 12600,
         .expectedPowerNowW = -20.0,
         .expectedChargeNowWh = 30.0,
         .expectedVoltageMv = 12600},
    Case{.name = "full",
         .state = BatteryState::Full,
         .rateW = 0.0,
         .remainingWh = 50.0,
         .fullWh = 50.0,
         .voltageMv = 12900,
         .expectedPowerNowW = 0.0,
         .expectedChargeNowWh = 50.0,
         .expectedVoltageMv = 12900},
    // On AC with the battery neither charging nor discharging: a reported 0 W rate, not a missing one.
    Case{.name = "idle",
         .state = BatteryState::NotCharging,
         .rateW = 0.0,
         .remainingWh = 35.0,
         .fullWh = 50.0,
         .voltageMv = 12300,
         .expectedPowerNowW = 0.0,
         .expectedChargeNowWh = 35.0,
         .expectedVoltageMv = 12300},
    Case{.name = "no voltage",
         .state = BatteryState::Discharging,
         .rateW = 8.0,
         .remainingWh = 25.0,
         .fullWh = 50.0,
         .reportsVoltage = false,
         .expectedPowerNowW = 8.0,
         .expectedChargeNowWh = 25.0,
         .expectedVoltageMv = 0},
    Case{.name = "no remaining charge",
         .state = BatteryState::Discharging,
         .rateW = 6.0,
         .reportsCharge = false,
         .voltageMv = 11000,
         .expectedPowerNowW = 6.0,
         .expectedChargeNowWh = 0.0,
         .expectedVoltageMv = 11000},
    Case{.name = "no rate",
         .state = BatteryState::Discharging,
         .reportsRate = false,
         .remainingWh = 45.0,
         .fullWh = 50.0,
         .voltageMv = 12000,
         .expectedPowerNowW = 0.0,
         .expectedChargeNowWh = 45.0,
         .expectedVoltageMv = 12000},
};

} // namespace TestSupport::BatteryRead
