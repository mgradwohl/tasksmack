#pragma once

#include "Platform/CpuDetails.h"
#include "Platform/ISystemProbe.h"
#include "Platform/Linux/LinuxCpuDetails.h"
#include "Platform/SystemTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
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

    /// Testability constructor that also takes the CPU sysfs root (normally /sys/devices/system/cpu),
    /// where the CPU frequency is read (#1183), and the CPU Details' caches and base clock (#809).
    LinuxSystemProbe(std::filesystem::path procRoot, std::filesystem::path sysClassNetRoot, std::filesystem::path cpuSysfsRoot);

    /// Testability constructor that also takes the file system root (normally /) under which the
    /// CPU Details' virtualization status finds /dev/kvm and the hypervisor hints (DMI, WSL's kernel
    /// release; LinuxOsInfo::runsUnderHypervisor()), read once here (#2592).
    LinuxSystemProbe(std::filesystem::path procRoot,
                     std::filesystem::path sysClassNetRoot,
                     std::filesystem::path cpuSysfsRoot,
                     const std::filesystem::path& rootDir);

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
    /// /proc/pressure/{cpu,memory,io} into the counters (#3062). Only called when m_HasPressure.
    static void readPressure(SystemCounters& counters, const std::filesystem::path& procRoot);
    /// @p procRoot/pressure/<resource>, parsed; empty when it can't be read (no PSI, or psi=0).
    [[nodiscard]] static ResourcePressure readResourcePressure(const std::filesystem::path& procRoot, const char* resource);
    static void readCpuFreq(SystemCounters& counters, const std::filesystem::path& cpuSysfsRoot);
    /// Every zram device's mm_stat, summed into the counters (#3087); the device list is re-read from
    /// m_SysBlockRoot every ZRAM_RESCAN_INTERVAL_SECONDS.
    void readZram(SystemCounters& counters);

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

    /// The link kind of <sysClassNetRoot>/<iface> (#2568): its uevent DEVTYPE where that names one
    /// (wlan, bluetooth, wwan, wireguard), then a `wireless` directory or `phy80211` link (Wi-Fi),
    /// then its `type`, the ARPHRD_* number (Ethernet, loopback, tunnels, raw-IP modems). Unknown
    /// when none of them can be read.
    [[nodiscard]] static NetInterfaceKind readInterfaceKind(const std::filesystem::path& sysClassNetRoot, std::string_view ifaceName);

    /// Read interface operational state from <sysClassNetRoot>/<iface>/operstate (up/down/unknown).
    [[nodiscard]] static bool readInterfaceOperState(const std::filesystem::path& sysClassNetRoot, std::string_view ifaceName);

    /// Read link speed directly from <sysClassNetRoot>/<iface>/speed (uncached).
    [[nodiscard]] static uint64_t readInterfaceLinkSpeedFromSysfs(const std::filesystem::path& sysClassNetRoot, std::string_view ifaceName);

    /// Remove cache entries for interfaces that no longer exist.
    /// @param currentInterfaces Vector of interface names seen in current enumeration
    void cleanupStaleInterfaceCacheEntries(const std::vector<std::string>& currentInterfaces);

    std::filesystem::path m_ProcRoot;
    std::filesystem::path m_SysClassNetRoot;
    std::filesystem::path m_CpuSysfsRoot;
    std::filesystem::path m_SysBlockRoot; // <rootDir>/sys/block, where zram devices are (#3087)
    long m_TicksPerSecond;
    std::size_t m_NumCores;
    bool m_HasPressure = false; // the kernel has PSI enabled: /proc/pressure/cpu reads (#3062)

    // Cached static info (read once)
    std::string m_Hostname;
    std::string m_CpuModel;
    // Topology, caches and base clock from /proc/cpuinfo and the CPU sysfs root (#809), with the CPU
    // numbers they describe; re-read when the set of CPUs in /proc/stat changes (a CPU brought
    // online or offline). m_LastCoreCount is the last non-empty /proc/stat CPU count, published as
    // cpuCoreCount when a sample's per-core read fails. Both guarded by m_CpuDetailsMutex: read()
    // may run on several threads.
    std::mutex m_CpuDetailsMutex;
    CpuTopology::CachedCpuDetails m_CpuDetails;
    // Virtualization status (#2592), read once at construction and copied onto each read of the
    // details above. Not changed after construction, so it needs no lock.
    LinuxCpuDetails::CpuVirtualization m_CpuVirtualization;
    std::size_t m_LastCoreCount = 0;

    // The zram devices' mm_stat paths (#3087) and when /sys/block was last listed; guarded by
    // m_ZramMutex, since read() may run on several threads.
    std::mutex m_ZramMutex;
    std::vector<std::filesystem::path> m_ZramStatPaths;
    std::chrono::steady_clock::time_point m_ZramScannedAt;
    bool m_ZramScanned = false;

    /// Publish this sample's CPU details and core count (#809), in one critical section, so a
    /// concurrent read() cannot pair this sample's per-core counters with details or a count from
    /// another set of CPUs. When /proc/stat's CPUs (counters.cpuPerCore's core ids) differ from the
    /// ones the details describe, the details are re-read and committed only if the re-read
    /// describes exactly this sample's CPUs (CpuTopology::commitIfConsistent()); otherwise the
    /// previous details are published and the next sample retries. cpuCoreCount is this sample's
    /// per-core count, or m_LastCoreCount when the per-core read failed.
    void readCpuDetails(SystemCounters& counters);

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

    // Virtual/hardware classification and link kind per interface (#1335, #2568): isVirtualInterface()
    // and readInterfaceKind() cost a few sysfs lookups per interface, and an interface's class can't
    // change while it exists. Valid for the
    // interface set in m_ClassifiedInterfaces (the /proc/net/dev names, in order); any change to the
    // set re-classifies every interface, so a name that is removed and re-added -- possibly as a
    // different kind of interface -- is looked up again. Interfaces that couldn't be classified
    // (nullopt, #1260) aren't cached and are retried on every read. Guarded by m_InterfaceCacheMutex.
    /// An interface's classification: whether it is virtual, and its link kind (#2568).
    struct InterfaceClass
    {
        bool isVirtual = false;
        NetInterfaceKind kind = NetInterfaceKind::Unknown;
    };
    std::vector<std::string> m_ClassifiedInterfaces;
    std::unordered_map<std::string, InterfaceClass> m_InterfaceClass;

    /// Classify each of `names` as virtual or hardware, with its link kind (nullopt: can't tell),
    /// from the cache where the interface set is unchanged. Returns whether the set changed since
    /// the last call.
    bool classifyInterfaces(const std::vector<std::string>& names, std::vector<std::optional<InterfaceClass>>& classes);
};

} // namespace Platform
