#pragma once

#include "Platform/IPowerProbe.h"

#include <cstdint>
#include <optional>
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
    explicit LinuxPowerProbe(std::string powerSupplyRoot = "/sys/class/power_supply");
    ~LinuxPowerProbe() override = default;

    LinuxPowerProbe(const LinuxPowerProbe&) = delete;
    LinuxPowerProbe& operator=(const LinuxPowerProbe&) = delete;
    LinuxPowerProbe(LinuxPowerProbe&&) noexcept = default;
    LinuxPowerProbe& operator=(LinuxPowerProbe&&) noexcept = default;

    [[nodiscard]] PowerCounters read() override;
    [[nodiscard]] PowerCapabilities capabilities() const override;

  private:
    /// The figures a capacity is weighted by when packs are combined (#2319): Wh when the pack gives
    /// energy, or charge plus a voltage; its raw charge (µAh) when it gives charge but no voltage.
    enum class CapacityUnit : std::uint8_t
    {
        None,
        WattHours,
        MicroAmpHours,
    };

    /// One pack's reading before the packs are combined into the system's battery (#2319)
    struct PackReading
    {
        PowerCounters counters; // This pack alone; no health or time estimates yet
        CapacityUnit unit = CapacityUnit::None;
        double now = 0.0; // In unit
        double full = 0.0;
        double design = 0.0;
        double healthFull = 0.0; // The raw full/design pair health comes from, either unit
        double healthDesign = 0.0;
    };

    void discoverBatteries();
    void addPackCapabilities(const std::string& batteryPath);
    /// The pack at batteryPath, or nullopt when it is absent (`present` is 0) or has gone
    [[nodiscard]] static std::optional<PackReading> readPack(const std::string& batteryPath);
    /// The packs as one system battery; see the definition for the policy
    [[nodiscard]] static PowerCounters combinePacks(const std::vector<PackReading>& packs);
    [[nodiscard]] static std::string readSysfsFile(const std::string& path);
    [[nodiscard]] static std::uint64_t readSysfsUInt64(const std::string& path, std::uint64_t fallback = 0);
    [[nodiscard]] static int readSysfsInt(const std::string& path, int fallback = -1);
    [[nodiscard]] static std::int64_t readSysfsInt64(const std::string& path, std::int64_t fallback = 0);
    /// Whether an AC adapter reports power: true if any is online, false if all report offline,
    /// nullopt when there is none or none can be read. Adapters are listed on every call.
    [[nodiscard]] std::optional<bool> readMainsOnline() const;

    std::vector<std::string> m_BatteryPaths; // The system's packs, by name, like "/sys/class/power_supply/BAT0"
    std::string m_PowerSupplyRoot;           // Root sysfs path (injectable for testing)
    PowerCapabilities m_Capabilities;
    bool m_Discovered = false; // discoverBatteries() has run once
};

} // namespace Platform
