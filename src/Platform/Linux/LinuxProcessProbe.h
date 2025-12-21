#pragma once

#include "Platform/IProcessProbe.h"

namespace Platform
{

/// Linux implementation of IProcessProbe.
/// Reads from /proc filesystem.
class LinuxProcessProbe : public IProcessProbe
{
  public:
    LinuxProcessProbe();
    ~LinuxProcessProbe() override = default;

    LinuxProcessProbe(const LinuxProcessProbe&) = delete;
    LinuxProcessProbe& operator=(const LinuxProcessProbe&) = delete;
    LinuxProcessProbe(LinuxProcessProbe&&) = default;
    LinuxProcessProbe& operator=(LinuxProcessProbe&&) = default;

    [[nodiscard]] std::vector<ProcessCounters> enumerate() override;
    [[nodiscard]] ProcessCapabilities capabilities() const override;
    [[nodiscard]] uint64_t totalCpuTime() const override;
    [[nodiscard]] long ticksPerSecond() const override;
    [[nodiscard]] uint64_t systemTotalMemory() const override;

  private:
    long m_TicksPerSecond;
    uint64_t m_PageSize;

    /// Parse /proc/[pid]/stat for a single process
    [[nodiscard]] bool parseProcessStat(int32_t pid, ProcessCounters& counters) const;

    /// Parse /proc/[pid]/statm for memory info
    void parseProcessStatm(int32_t pid, ProcessCounters& counters) const;

    /// Parse /proc/[pid]/status for owner (UID) info
    void parseProcessStatus(int32_t pid, ProcessCounters& counters) const;

    /// Parse /proc/[pid]/cmdline for full command line
    void parseProcessCmdline(int32_t pid, ProcessCounters& counters) const;

    /// Read total CPU time from /proc/stat
    [[nodiscard]] uint64_t readTotalCpuTime() const;
};

} // namespace Platform
