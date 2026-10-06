#pragma once

#include "Platform/ISystemProbe.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Platform
{

/// Linux implementation of ISystemProbe.
/// Reads system metrics from /proc/stat, /proc/meminfo, /proc/uptime.
class LinuxSystemProbe : public ISystemProbe
{
  public:
    LinuxSystemProbe();

    /// Testability constructor: reads from a custom proc root instead of /proc.
    /// Useful for unit tests that supply synthetic /proc content.
    explicit LinuxSystemProbe(std::filesystem::path procRoot);

    /// Testability constructor that also takes the network-interface sysfs root (normally
    /// /sys/class/net), where interfaces are classified as hardware or virtual (#1106).
    LinuxSystemProbe(std::filesystem::path procRoot, std::filesystem::path sysClassNetRoot);

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
    static void readCpuCounters(SystemCounters& counters, const std::filesystem::path& procRoot);
    static void readMemoryCounters(SystemCounters& counters, const std::filesystem::path& procRoot);
    static void readUptime(SystemCounters& counters, const std::filesystem::path& procRoot);
    static void readLoadAvg(SystemCounters& counters, const std::filesystem::path& procRoot);
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

    /// True when the interface has no backing device (no <sysClassNetRoot>/<iface>/device; it lives
    /// under /sys/devices/virtual/net): loopback, bridges, veth pairs, VLANs, bonds, tun/tap,
    /// WireGuard. Their traffic also crosses a hardware interface, so the Total leaves them out (#1106).
    /// nullopt when the interface isn't in sysfs at all (not mounted, or it vanished): it can't be
    /// classified, and is counted as hardware (#1260).
    [[nodiscard]] static std::optional<bool> isVirtualInterface(const std::filesystem::path& sysClassNetRoot, std::string_view ifaceName);

    /// Read interface operational state from sysfs (up/down/unknown).
    [[nodiscard]] static bool readInterfaceOperState(const std::string& ifaceName);

    /// Read link speed directly from sysfs (uncached).
    [[nodiscard]] static uint64_t readInterfaceLinkSpeedFromSysfs(const std::string& ifaceName);

    /// Remove cache entries for interfaces that no longer exist.
    /// @param currentInterfaces Vector of interface names seen in current enumeration
    void cleanupStaleInterfaceCacheEntries(const std::vector<std::string>& currentInterfaces);

    std::filesystem::path m_ProcRoot;
    std::filesystem::path m_SysClassNetRoot;
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
        std::chrono::steady_clock::time_point lastSpeedCheck;
    };
    std::mutex m_InterfaceCacheMutex;
    std::unordered_map<std::string, InterfaceCacheEntry> m_InterfaceCache;
};

} // namespace Platform
