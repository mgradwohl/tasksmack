#pragma once

#include "Platform/ISystemProbe.h"

#include <chrono>
#include <cstddef>
#include <mutex>
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

    // Non-copyable, non-movable (contains mutex)
    LinuxSystemProbe(const LinuxSystemProbe&) = delete;
    LinuxSystemProbe& operator=(const LinuxSystemProbe&) = delete;
    LinuxSystemProbe(LinuxSystemProbe&&) = delete;
    LinuxSystemProbe& operator=(LinuxSystemProbe&&) = delete;

    [[nodiscard]] SystemCounters read() override;
    [[nodiscard]] SystemCapabilities capabilities() const override;
    [[nodiscard]] long ticksPerSecond() const override;

  private:
    static void readCpuCounters(SystemCounters& counters);
    static void readMemoryCounters(SystemCounters& counters);
    static void readUptime(SystemCounters& counters);
    static void readLoadAvg(SystemCounters& counters);
    static void readCpuFreq(SystemCounters& counters);

    /// Read network-related counters (bytes, packets, etc.) from /proc/net/dev.
    /// Unlike the other read* helpers, this method is non-static because it
    /// uses m_InterfaceCache to cache per-interface link speed and state in
    /// order to avoid repeated sysfs reads. The other helpers are stateless
    /// and remain static.
    void readNetworkCounters(SystemCounters& counters);
    void readStaticInfo(SystemCounters& counters) const;

    /// Get interface link speed (returns 0 if unavailable).
    /// Uses cache to reduce sysfs I/O - link speed rarely changes.
    /// @param ifaceName Interface name (e.g., "eth0", "wlan0")
    /// @param isUp Current operational state (for detecting down→up transitions)
    [[nodiscard]] uint64_t getInterfaceLinkSpeed(const std::string& ifaceName, bool isUp);

    /// Read interface operational state from sysfs (up/down/unknown).
    [[nodiscard]] static bool readInterfaceOperState(const std::string& ifaceName);

    /// Read link speed directly from sysfs (uncached).
    [[nodiscard]] static uint64_t readInterfaceLinkSpeedFromSysfs(const std::string& ifaceName);

    /// Remove cache entries for interfaces that no longer exist.
    /// @param currentInterfaces Vector of interface names seen in current enumeration
    void cleanupStaleInterfaceCacheEntries(const std::vector<std::string>& currentInterfaces);

    long m_TicksPerSecond;
    std::size_t m_NumCores;

    // Cached static info (read once)
    std::string m_Hostname;
    std::string m_CpuModel;

    // Optimization cache for network interface properties.
    // NOTE: This is NOT semantic state - the probe contract remains stateless (raw counters).
    // This is analogous to m_Hostname and m_CpuModel above: caching values that rarely change
    // (link speed only changes on cable replug or driver reload) to reduce sysfs I/O overhead.
    // The cached values don't affect correctness, only performance.
    // Protected by m_InterfaceCacheMutex for thread safety.
    struct InterfaceCacheEntry
    {
        uint64_t linkSpeedMbps = 0;
        bool wasUp = false; // Track state transitions
        std::chrono::steady_clock::time_point lastSpeedCheck{};
    };
    mutable std::mutex m_InterfaceCacheMutex;
    std::unordered_map<std::string, InterfaceCacheEntry> m_InterfaceCache;

    static constexpr int64_t LINK_SPEED_CACHE_TTL_SECONDS = 60;
};

} // namespace Platform
