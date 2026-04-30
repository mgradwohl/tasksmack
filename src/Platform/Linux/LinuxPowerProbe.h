#pragma once

#include "Platform/IPowerProbe.h"

#include <string>
#include <vector>

namespace Platform
{

/// Linux implementation of IPowerProbe.
/// Reads power/battery metrics from /sys/class/power_supply.
/// An optional custom root path can be provided for unit-testing with
/// a synthetic sysfs directory tree instead of the real /sys filesystem.
class LinuxPowerProbe : public IPowerProbe
{
  public:
    explicit LinuxPowerProbe(std::string powerSupplyRoot = std::string("/sys/class/power_supply"));
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
    [[nodiscard]] static std::string readSysfsFile(const std::string& path);
    [[nodiscard]] static std::uint64_t readSysfsUInt64(const std::string& path, std::uint64_t fallback = 0);
    [[nodiscard]] static int readSysfsInt(const std::string& path, int fallback = -1);

    std::vector<std::string> m_BatteryPaths;  // Paths like "/sys/class/power_supply/BAT0"
    std::string m_PowerSupplyRoot;            // Root sysfs path (injectable for testing)
    PowerCapabilities m_Capabilities;
};

} // namespace Platform
