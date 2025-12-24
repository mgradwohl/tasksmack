#pragma once

#include "Platform/IPowerProbe.h"

#include <string>
#include <vector>

namespace Platform
{

/// Linux implementation of IPowerProbe.
/// Reads power/battery metrics from /sys/class/power_supply.
class LinuxPowerProbe : public IPowerProbe
{
  public:
    LinuxPowerProbe();
    ~LinuxPowerProbe() override = default;

    LinuxPowerProbe(const LinuxPowerProbe&) = delete;
    LinuxPowerProbe& operator=(const LinuxPowerProbe&) = delete;
    LinuxPowerProbe(LinuxPowerProbe&&) = default;
    LinuxPowerProbe& operator=(LinuxPowerProbe&&) = default;

    [[nodiscard]] PowerCounters read() override;
    [[nodiscard]] PowerCapabilities capabilities() const override;

  private:
    void discoverBatteries();
    void readBattery(PowerCounters& counters, const std::string& batteryPath) const;
    [[nodiscard]] std::string readSysfsFile(const std::string& path) const;
    [[nodiscard]] std::uint64_t readSysfsUInt64(const std::string& path, std::uint64_t fallback = 0) const;
    [[nodiscard]] int readSysfsInt(const std::string& path, int fallback = -1) const;

    std::vector<std::string> m_BatteryPaths; // Paths like "/sys/class/power_supply/BAT0"
    PowerCapabilities m_Capabilities;
};

} // namespace Platform
