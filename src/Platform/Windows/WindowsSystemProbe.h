#pragma once

#include "Platform/ISystemProbe.h"

#include <cstddef>
#include <string>

namespace Platform
{

/// Windows implementation of ISystemProbe.
/// Reads system metrics from Windows APIs (GetSystemTimes, GlobalMemoryStatusEx, etc).
class WindowsSystemProbe : public ISystemProbe
{
  public:
    WindowsSystemProbe();
    ~WindowsSystemProbe() override = default;

    WindowsSystemProbe(const WindowsSystemProbe&) = delete;
    WindowsSystemProbe& operator=(const WindowsSystemProbe&) = delete;
    WindowsSystemProbe(WindowsSystemProbe&&) = default;
    WindowsSystemProbe& operator=(WindowsSystemProbe&&) = default;

    [[nodiscard]] SystemCounters read() override;
    [[nodiscard]] SystemCapabilities capabilities() const override;
    [[nodiscard]] long ticksPerSecond() const override;

  private:
    void readCpuCounters(SystemCounters& counters) const;
    void readPerCoreCpuCounters(SystemCounters& counters) const;
    void readMemoryCounters(SystemCounters& counters) const;
    void readUptime(SystemCounters& counters) const;
    void readStaticInfo(SystemCounters& counters) const;
    void readCpuFreq(SystemCounters& counters) const;

    std::size_t m_NumCores{0};

    // Cached static info (read once)
    std::string m_Hostname;
    std::string m_CpuModel;
};

} // namespace Platform
