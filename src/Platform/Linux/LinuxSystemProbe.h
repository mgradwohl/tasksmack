#pragma once

#include "Platform/ISystemProbe.h"

namespace Platform
{

/// Linux implementation of ISystemProbe.
/// Reads system metrics from /proc/stat, /proc/meminfo, /proc/uptime.
class LinuxSystemProbe : public ISystemProbe
{
  public:
    LinuxSystemProbe();

    [[nodiscard]] SystemCounters read() override;
    [[nodiscard]] SystemCapabilities capabilities() const override;
    [[nodiscard]] long ticksPerSecond() const override;

  private:
    void readCpuCounters(SystemCounters& counters) const;
    void readMemoryCounters(SystemCounters& counters) const;
    void readUptime(SystemCounters& counters) const;

    long m_TicksPerSecond;
    int m_NumCores;
};

} // namespace Platform
