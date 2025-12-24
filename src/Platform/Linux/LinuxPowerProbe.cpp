// Keep this translation unit parseable on non-Linux platforms (e.g. Windows clangd)
// by compiling the implementation only when targeting Linux and required headers exist.
#if defined(__linux__) && __has_include(<unistd.h>)

#include "LinuxPowerProbe.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace Platform
{

namespace
{

constexpr const char* POWER_SUPPLY_PATH = "/sys/class/power_supply";

[[nodiscard]] bool isBatteryDevice(const std::string& devicePath)
{
    std::ifstream typeFile(devicePath + "/type");
    if (!typeFile.is_open())
    {
        return false;
    }

    std::string type;
    std::getline(typeFile, type);
    return type == "Battery";
}

[[nodiscard]] BatteryState parseBatteryState(const std::string& status)
{
    if (status == "Charging")
    {
        return BatteryState::Charging;
    }
    if (status == "Discharging")
    {
        return BatteryState::Discharging;
    }
    if (status == "Full")
    {
        return BatteryState::Full;
    }
    if (status == "Not charging")
    {
        return BatteryState::Full; // Treat as full if plugged but not charging
    }
    return BatteryState::Unknown;
}

} // namespace

LinuxPowerProbe::LinuxPowerProbe()
{
    discoverBatteries();
    spdlog::debug("LinuxPowerProbe: found {} batteries", m_BatteryPaths.size());
}

void LinuxPowerProbe::discoverBatteries()
{
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(POWER_SUPPLY_PATH, ec) || !fs::is_directory(POWER_SUPPLY_PATH, ec))
    {
        spdlog::debug("LinuxPowerProbe: {} not found or not a directory", POWER_SUPPLY_PATH);
        m_Capabilities.hasBattery = false;
        return;
    }

    for (const auto& entry : fs::directory_iterator(POWER_SUPPLY_PATH, ec))
    {
        if (!entry.is_directory(ec))
        {
            continue;
        }

        const auto devicePath = entry.path().string();
        if (isBatteryDevice(devicePath))
        {
            m_BatteryPaths.push_back(devicePath);
            spdlog::debug("LinuxPowerProbe: discovered battery at {}", devicePath);
        }
    }

    if (ec)
    {
        spdlog::warn("LinuxPowerProbe: error iterating {}: {}", POWER_SUPPLY_PATH, ec.message());
    }

    // Set capabilities based on discovery
    m_Capabilities.hasBattery = !m_BatteryPaths.empty();
    if (m_Capabilities.hasBattery)
    {
        // Probe one battery to determine available fields
        const auto& batteryPath = m_BatteryPaths[0];
        m_Capabilities.hasChargePercent = fs::exists(batteryPath + "/capacity", ec);
        m_Capabilities.hasChargeCapacity = fs::exists(batteryPath + "/energy_now", ec) || fs::exists(batteryPath + "/charge_now", ec);
        m_Capabilities.hasPowerRate = fs::exists(batteryPath + "/power_now", ec) || fs::exists(batteryPath + "/current_now", ec);
        m_Capabilities.hasVoltage = fs::exists(batteryPath + "/voltage_now", ec);
        m_Capabilities.hasTechnology = fs::exists(batteryPath + "/technology", ec);
        m_Capabilities.hasCycleCount = fs::exists(batteryPath + "/cycle_count", ec);
        m_Capabilities.hasHealthPercent =
            fs::exists(batteryPath + "/energy_full", ec) && fs::exists(batteryPath + "/energy_full_design", ec);
        m_Capabilities.hasTimeEstimates = false; // Linux doesn't provide time estimates directly
    }
}

PowerCounters LinuxPowerProbe::read()
{
    PowerCounters counters;

    if (m_BatteryPaths.empty())
    {
        counters.state = BatteryState::NotPresent;
        counters.isOnAc = true; // Assume on AC if no battery
        return counters;
    }

    // For simplicity, read from the first battery (most systems have one)
    // Future: aggregate multiple batteries
    readBattery(counters, m_BatteryPaths[0]);

    return counters;
}

PowerCapabilities LinuxPowerProbe::capabilities() const
{
    return m_Capabilities;
}

void LinuxPowerProbe::readBattery(PowerCounters& counters, const std::string& batteryPath) const
{
    // Read battery state
    const auto status = readSysfsFile(batteryPath + "/status");
    counters.state = parseBatteryState(status);
    counters.isOnAc = (counters.state == BatteryState::Charging || counters.state == BatteryState::Full);

    // Read charge percentage
    if (m_Capabilities.hasChargePercent)
    {
        counters.chargePercent = readSysfsInt(batteryPath + "/capacity", -1);
    }

    // Read charge capacity (energy or charge, in µWh or µAh)
    // Convert to Wh for consistency
    if (m_Capabilities.hasChargeCapacity)
    {
        // Try energy first (Wh)
        if (std::filesystem::exists(batteryPath + "/energy_now"))
        {
            counters.chargeNowWh = readSysfsUInt64(batteryPath + "/energy_now") / 1000000; // µWh to Wh
            counters.chargeFullWh = readSysfsUInt64(batteryPath + "/energy_full") / 1000000;
            counters.chargeDesignWh = readSysfsUInt64(batteryPath + "/energy_full_design") / 1000000;
        }
        // Fall back to charge (Ah) - need voltage to convert to Wh
        else if (std::filesystem::exists(batteryPath + "/charge_now"))
        {
            const auto chargeNowUah = readSysfsUInt64(batteryPath + "/charge_now");
            const auto chargeFullUah = readSysfsUInt64(batteryPath + "/charge_full");
            const auto chargeDesignUah = readSysfsUInt64(batteryPath + "/charge_full_design");
            const auto voltageUv = readSysfsUInt64(batteryPath + "/voltage_now", 0);

            if (voltageUv > 0)
            {
                // Convert µAh * µV = pWh, then to Wh
                // Cast to double to prevent overflow before division
                counters.chargeNowWh = (static_cast<double>(chargeNowUah) * static_cast<double>(voltageUv)) / 1000000000000.0;
                counters.chargeFullWh = (static_cast<double>(chargeFullUah) * static_cast<double>(voltageUv)) / 1000000000000.0;
                counters.chargeDesignWh = (static_cast<double>(chargeDesignUah) * static_cast<double>(voltageUv)) / 1000000000000.0;
            }
        }
    }

    // Read power rate
    if (m_Capabilities.hasPowerRate)
    {
        // Try power_now first (µW)
        if (std::filesystem::exists(batteryPath + "/power_now"))
        {
            const auto powerUw = readSysfsUInt64(batteryPath + "/power_now");
            counters.powerNowW = static_cast<double>(powerUw) / 1000000.0; // µW to W

            // Negate if charging (power going in)
            if (counters.state == BatteryState::Charging)
            {
                counters.powerNowW = -counters.powerNowW;
            }
        }
        // Fall back to current_now (µA) - need voltage
        else if (std::filesystem::exists(batteryPath + "/current_now"))
        {
            const auto currentUa = readSysfsUInt64(batteryPath + "/current_now");
            const auto voltageUv = readSysfsUInt64(batteryPath + "/voltage_now", 0);

            if (voltageUv > 0)
            {
                // µA * µV = pW, then to W
                // Cast to double to prevent overflow before division
                counters.powerNowW = (static_cast<double>(currentUa) * static_cast<double>(voltageUv)) / 1000000000000.0;

                if (counters.state == BatteryState::Charging)
                {
                    counters.powerNowW = -counters.powerNowW;
                }
            }
        }
    }

    // Read voltage
    if (m_Capabilities.hasVoltage)
    {
        counters.voltageNowMv = readSysfsUInt64(batteryPath + "/voltage_now") / 1000; // µV to mV
    }

    // Read technology
    if (m_Capabilities.hasTechnology)
    {
        counters.technology = readSysfsFile(batteryPath + "/technology");
    }

    // Read model/manufacturer
    counters.model = readSysfsFile(batteryPath + "/model_name");
    counters.manufacturer = readSysfsFile(batteryPath + "/manufacturer");

    // Read cycle count
    if (m_Capabilities.hasCycleCount)
    {
        counters.cycleCount = readSysfsUInt64(batteryPath + "/cycle_count");
    }

    // Calculate health percentage
    if (m_Capabilities.hasHealthPercent && counters.chargeDesignWh > 0)
    {
        counters.healthPercent = static_cast<int>((counters.chargeFullWh * 100) / counters.chargeDesignWh);
    }

    // Estimate time remaining (rough calculation)
    if (counters.powerNowW > 0.1 && counters.chargeNowWh > 0)
    {
        // Time to empty = charge / power
        counters.timeToEmptySec = static_cast<std::uint64_t>((counters.chargeNowWh / counters.powerNowW) * 3600.0);
    }
    else if (counters.powerNowW < -0.1 && counters.chargeFullWh > counters.chargeNowWh)
    {
        // Time to full = (full - now) / power
        const auto remaining = counters.chargeFullWh - counters.chargeNowWh;
        counters.timeToFullSec = static_cast<std::uint64_t>((remaining / -counters.powerNowW) * 3600.0);
    }
}

std::string LinuxPowerProbe::readSysfsFile(const std::string& path) const
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        return "";
    }

    std::string value;
    std::getline(file, value);

    // Trim whitespace efficiently
    if (!value.empty())
    {
        // Trim trailing whitespace
        const auto lastNonWs = value.find_last_not_of(" \n\r\t");
        if (lastNonWs == std::string::npos)
        {
            return ""; // String contains only whitespace
        }
        value.resize(lastNonWs + 1);

        // Trim leading spaces
        const auto firstNonSpace = value.find_first_not_of(' ');
        if (firstNonSpace != std::string::npos && firstNonSpace > 0)
        {
            value.erase(0, firstNonSpace);
        }
    }

    return value;
}

std::uint64_t LinuxPowerProbe::readSysfsUInt64(const std::string& path, std::uint64_t fallback) const
{
    const auto str = readSysfsFile(path);
    if (str.empty())
    {
        return fallback;
    }

    std::uint64_t value = 0;
    const auto result = std::from_chars(str.data(), str.data() + str.size(), value);
    if (result.ec != std::errc{})
    {
        return fallback;
    }

    return value;
}

int LinuxPowerProbe::readSysfsInt(const std::string& path, int fallback) const
{
    const auto str = readSysfsFile(path);
    if (str.empty())
    {
        return fallback;
    }

    int value = 0;
    const auto result = std::from_chars(str.data(), str.data() + str.size(), value);
    if (result.ec != std::errc{})
    {
        return fallback;
    }

    return value;
}

} // namespace Platform

#endif // defined(__linux__) && __has_include(<unistd.h>)
