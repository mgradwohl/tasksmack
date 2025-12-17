#pragma once

#include "Platform/IProcessProbe.h"

namespace Platform
{

/// Windows implementation of IProcessProbe.
/// Uses ToolHelp32 API and GetProcessTimes/GetProcessMemoryInfo.
class WindowsProcessProbe : public IProcessProbe
{
  public:
    WindowsProcessProbe();
    ~WindowsProcessProbe() override = default;

    WindowsProcessProbe(const WindowsProcessProbe&) = delete;
    WindowsProcessProbe& operator=(const WindowsProcessProbe&) = delete;
    WindowsProcessProbe(WindowsProcessProbe&&) = default;
    WindowsProcessProbe& operator=(WindowsProcessProbe&&) = default;

    [[nodiscard]] std::vector<ProcessCounters> enumerate() override;
    [[nodiscard]] ProcessCapabilities capabilities() const override;
    [[nodiscard]] uint64_t totalCpuTime() const override;
    [[nodiscard]] long ticksPerSecond() const override;
    [[nodiscard]] uint64_t systemTotalMemory() const override;

  private:
    /// Get detailed info for a single process
    [[nodiscard]] bool getProcessDetails(uint32_t pid, ProcessCounters& counters) const;

    /// Read total system CPU time
    [[nodiscard]] uint64_t readTotalCpuTime() const;
};

} // namespace Platform
