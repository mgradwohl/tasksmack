#pragma once

#include <cstdint>
#include <string>

namespace Platform
{

/// Battery/power supply state
enum class BatteryState : std::uint8_t
{
    Unknown = 0,
    Charging,
    Discharging,
    Full,
    NotPresent
};

/// Raw power/battery counters from OS.
/// Probes populate this; domain may compute rates or trends.
struct PowerCounters
{
    // Battery state
    BatteryState state = BatteryState::Unknown;
    bool isOnAc = false; // Connected to AC power

    // Battery percentage (0-100, or -1 if unavailable)
    int chargePercent = -1;

    // Battery charge/capacity in Wh or mWh (platform dependent)
    std::uint64_t chargeNowWh = 0;
    std::uint64_t chargeFullWh = 0;
    std::uint64_t chargeDesignWh = 0;

    // Power consumption/rate in Watts or milliwatts (platform dependent)
    // Positive = discharging/consuming, negative = charging
    double powerNowW = 0.0;

    // Remaining time estimates in seconds (or 0 if unavailable)
    std::uint64_t timeToEmptySec = 0;
    std::uint64_t timeToFullSec = 0;

    // Voltage (mV)
    std::uint64_t voltageNowMv = 0;

    // Battery technology/chemistry (e.g., "Li-ion", "Li-poly")
    std::string technology;

    // Battery model/manufacturer
    std::string model;
    std::string manufacturer;

    // Cycle count (if available)
    std::uint64_t cycleCount = 0;

    // Health percentage (0-100, or -1 if unavailable)
    // Calculated as (charge_full / charge_full_design) * 100
    int healthPercent = -1;
};

/// Reports what this platform's power probe supports.
struct PowerCapabilities
{
    bool hasBattery = false;
    bool hasChargePercent = false;
    bool hasChargeCapacity = false;
    bool hasPowerRate = false;
    bool hasTimeEstimates = false;
    bool hasVoltage = false;
    bool hasTechnology = false;
    bool hasCycleCount = false;
    bool hasHealthPercent = false;
};

} // namespace Platform
