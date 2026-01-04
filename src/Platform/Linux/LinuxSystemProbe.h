#pragma once

#include "Platform/ISystemProbe.h"

#include <cstddef>
#include <string>

namespace Platform
{

/// Linux implementation of ISystemProbe.
/// Reads system metrics from /proc/stat, /proc/meminfo, /proc/uptime.
class LinuxSystemProbe : public ISystemProbe
{
  public:
    LinuxSystemProbe();
    ~LinuxSystemProbe() override = default;

    LinuxSystemProbe(const LinuxSystemProbe&) = delete;
    LinuxSystemProbe& operator=(const LinuxSystemProbe&) = delete;
    LinuxSystemProbe(LinuxSystemProbe&&) = default;
    LinuxSystemProbe& operator=(LinuxSystemProbe&&) = default;

    [[nodiscard]] SystemCounters read() override;
    [[nodiscard]] SystemCapabilities capabilities() const override;
    [[nodiscard]] long ticksPerSecond() const override;

  private:
    static void readCpuCounters(SystemCounters& counters);
    static void readMemoryCounters(SystemCounters& counters);
    static void readUptime(SystemCounters& counters);
    static void readLoadAvg(SystemCounters& counters);
    static void readCpuFreq(SystemCounters& counters);
    static void readNetworkCounters(SystemCounters& counters);
    void readStaticInfo(SystemCounters& counters) const;

    /// Read interface link speed from sysfs (returns 0 if unavailable).
    [[nodiscard]] static uint64_t readInterfaceLinkSpeed(const std::string& ifaceName);

    long m_TicksPerSecond;
    std::size_t m_NumCores;

    // Cached static info (read once)
    std::string m_Hostname;
    std::string m_CpuModel;
};

} // namespace Platform
