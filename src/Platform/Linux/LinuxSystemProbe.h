#pragma once

#include "Platform/ISystemProbe.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>

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
    void readNetworkCounters(SystemCounters& counters);
    void readStaticInfo(SystemCounters& counters) const;

    /// Read interface link speed from sysfs (returns 0 if unavailable).
    /// Uses cache to reduce sysfs I/O - link speed rarely changes.
    /// @param ifaceName Interface name (e.g., "eth0", "wlan0")
    /// @param isUp Current operational state (for detecting down→up transitions)
    [[nodiscard]] uint64_t readInterfaceLinkSpeed(const std::string& ifaceName, bool isUp);

    /// Read interface operational state from sysfs (up/down/unknown).
    [[nodiscard]] static bool readInterfaceOperState(const std::string& ifaceName);

    /// Read link speed directly from sysfs (uncached).
    [[nodiscard]] static uint64_t readInterfaceLinkSpeedFromSysfs(const std::string& ifaceName);

    long m_TicksPerSecond;
    std::size_t m_NumCores;

    // Cached static info (read once)
    std::string m_Hostname;
    std::string m_CpuModel;

    /// Cached network interface info to reduce sysfs reads.
    /// Link speed rarely changes (only on cable replug or driver reload).
    /// We refresh the cache:
    /// - On first access for an interface
    /// - When interface transitions from down to up
    /// - Every LINK_SPEED_CACHE_TTL_SECONDS
    struct InterfaceCacheEntry
    {
        uint64_t linkSpeedMbps = 0;
        bool wasUp = false; // Track state transitions
        std::chrono::steady_clock::time_point lastSpeedCheck{};
    };
    std::unordered_map<std::string, InterfaceCacheEntry> m_InterfaceCache;

    static constexpr int64_t LINK_SPEED_CACHE_TTL_SECONDS = 60;
};

} // namespace Platform
