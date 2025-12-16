#pragma once

#include "Platform/ISystemProbe.h"

namespace Platform
{

/// Windows implementation of ISystemProbe.
/// Reads system metrics from Windows APIs (GetSystemTimes, GlobalMemoryStatusEx, etc).
class WindowsSystemProbe : public ISystemProbe
{
  public:
    WindowsSystemProbe();

    [[nodiscard]] SystemCounters read() override;
    [[nodiscard]] SystemCapabilities capabilities() const override;
    [[nodiscard]] long ticksPerSecond() const override;

  private:
    void readCpuCounters(SystemCounters& counters) const;
    void readPerCoreCpuCounters(SystemCounters& counters) const;
    void readMemoryCounters(SystemCounters& counters) const;
    void readUptime(SystemCounters& counters) const;

    int m_NumCores;
};

} // namespace Platform
