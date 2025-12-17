#pragma once

#include "Platform/ISystemProbe.h"

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
    void readCpuCounters(SystemCounters& counters) const;
    void readMemoryCounters(SystemCounters& counters) const;
    void readUptime(SystemCounters& counters) const;
    void readLoadAvg(SystemCounters& counters) const;
    void readCpuFreq(SystemCounters& counters) const;
    void readStaticInfo(SystemCounters& counters) const;

    long m_TicksPerSecond;
    int m_NumCores;

    // Cached static info (read once)
    std::string m_Hostname;
    std::string m_CpuModel;
};

} // namespace Platform
