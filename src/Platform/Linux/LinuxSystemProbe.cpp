// Keep this translation unit parseable on non-Linux platforms (e.g. Windows clangd)
// by compiling the implementation only when targeting Linux and required headers exist.
#if defined(__linux__) && __has_include(<unistd.h>)

#include "LinuxSystemProbe.h"

#include "Domain/SamplingConfig.h"
#include "Platform/SystemTypes.h"
#include "ProcParsing.h"

#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include <unistd.h>

namespace Platform
{

namespace
{

template<std::integral T> [[nodiscard]] constexpr auto checkedPositiveToSizeT(T value, std::size_t fallback) noexcept -> std::size_t
{
    if constexpr (std::is_signed_v<T>)
    {
        if (value <= 0)
        {
            return fallback;
        }
    }
    else
    {
        if (value == 0)
        {
            return fallback;
        }
    }

    return static_cast<std::size_t>(value);
}

using ProcParsing::parseDouble;
using ProcParsing::parseNum;
using ProcParsing::readProcFile;
using ProcParsing::readProcFileFull;

} // namespace

LinuxSystemProbe::LinuxSystemProbe() : LinuxSystemProbe(std::filesystem::path("/proc"))
{}

LinuxSystemProbe::LinuxSystemProbe(std::filesystem::path procRoot)
    : m_ProcRoot(std::move(procRoot)),
      m_TicksPerSecond(sysconf(_SC_CLK_TCK)),
      m_NumCores(checkedPositiveToSizeT(sysconf(_SC_NPROCESSORS_ONLN), 1U))
{
    if (m_TicksPerSecond <= 0)
    {
        m_TicksPerSecond = 100; // Common default
        spdlog::warn("Failed to get CLK_TCK, using default: {}", m_TicksPerSecond);
    }

    // Read hostname (cached)
    std::array<char, 256> hostBuffer{};
    if (gethostname(hostBuffer.data(), hostBuffer.size()) == 0)
    {
        m_Hostname = hostBuffer.data();
    }
    else
    {
        m_Hostname = "unknown";
    }

    // Read CPU model from /proc/cpuinfo (cached)
    std::ifstream cpuInfo(m_ProcRoot / "cpuinfo");
    if (cpuInfo.is_open())
    {
        std::string line;
        while (std::getline(cpuInfo, line))
        {
            if (line.starts_with("model name"))
            {
                auto pos = line.find(':');
                if (pos != std::string::npos)
                {
                    m_CpuModel = line.substr(pos + 1);
                    // Trim leading whitespace
                    while (!m_CpuModel.empty() && m_CpuModel[0] == ' ')
                    {
                        m_CpuModel.erase(0, 1);
                    }
                }
                break;
            }
        }
    }
    if (m_CpuModel.empty())
    {
        m_CpuModel = "Unknown CPU";
    }

    spdlog::debug("LinuxSystemProbe: {} cores, {} ticks/sec, host={}, cpu={}", m_NumCores, m_TicksPerSecond, m_Hostname, m_CpuModel);
}

SystemCounters LinuxSystemProbe::read()
{
    SystemCounters counters;
    // Pre-reserve the per-core CPU vector to avoid reallocation on every refresh.
    // Core count is fixed at construction; reserving here eliminates ~log2(numCores)
    // vector growth reallocations per read() call.
    counters.cpuPerCore.reserve(m_NumCores);
    readCpuCounters(counters, m_ProcRoot);
    readMemoryCounters(counters, m_ProcRoot);
    readUptime(counters, m_ProcRoot);
    readLoadAvg(counters, m_ProcRoot);
    readCpuFreq(counters);
    readNetworkCounters(counters);
    readStaticInfo(counters);
    return counters;
}

SystemCapabilities LinuxSystemProbe::capabilities() const
{
    return SystemCapabilities{.hasPerCoreCpu = true,
                              .hasMemoryAvailable = true, // Modern kernels have MemAvailable
                              .hasSwap = true,
                              .hasUptime = true,
                              .hasIoWait = true,
                              .hasSteal = true,
                              .hasLoadAvg = true,
                              .hasCpuFreq = true,
                              .hasNetworkCounters = true};
}

long LinuxSystemProbe::ticksPerSecond() const
{
    return m_TicksPerSecond;
}

void LinuxSystemProbe::readCpuCounters(SystemCounters& counters, const std::filesystem::path& procRoot)
{
    // Format: /proc/stat
    // cpu  user nice system idle iowait irq softirq steal guest guest_nice
    // cpu0 user nice system idle iowait irq softirq steal guest guest_nice
    // cpu1 ...

    const auto statPath = procRoot / "stat";
    const std::string pathStr = statPath.string();

    // Read until EOF so per-core CPU lines are never truncated on high-core-count machines.
    const std::vector<char> buf = readProcFileFull(pathStr.c_str());
    const std::size_t len = buf.size();
    if (len == 0)
    {
        spdlog::warn("Failed to open {}", pathStr);
        return;
    }

    const char* p = buf.data();
    const char* const end = buf.data() + len;
    bool foundTotal = false;

    while ((end - p) >= 3 && p[0] == 'c' && p[1] == 'p' && p[2] == 'u')
    {
        // Find end of this line
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n')
        {
            ++lineEnd;
        }

        const char* q = p + 3; // advance past "cpu"
        // Aggregate line: "cpu " (or "cpu\t") — per-core line: "cpu0", "cpu1", …
        const bool isTotal = (q >= lineEnd || *q == ' ' || *q == '\t');

        // Skip past the label token to reach the first numeric field
        while (q < lineEnd && *q != ' ' && *q != '\t')
        {
            ++q;
        }

        CpuCounters cpu{};
        // Older kernels may lack trailing guest/guestNice fields;
        // partial reads are fine — unparsed fields stay zero-initialised.
        parseNum(q, lineEnd, cpu.user);
        parseNum(q, lineEnd, cpu.nice);
        parseNum(q, lineEnd, cpu.system);
        parseNum(q, lineEnd, cpu.idle);
        parseNum(q, lineEnd, cpu.iowait);
        parseNum(q, lineEnd, cpu.irq);
        parseNum(q, lineEnd, cpu.softirq);
        parseNum(q, lineEnd, cpu.steal);
        parseNum(q, lineEnd, cpu.guest);
        parseNum(q, lineEnd, cpu.guestNice);

        if (isTotal)
        {
            counters.cpuTotal = cpu;
            foundTotal = true;
        }
        else
        {
            counters.cpuPerCore.push_back(cpu);
        }

        p = (lineEnd < end) ? lineEnd + 1 : end;
    }

    if (!foundTotal)
    {
        spdlog::warn("Failed to parse aggregate CPU line from {}", pathStr);
    }
}

void LinuxSystemProbe::readMemoryCounters(SystemCounters& counters, const std::filesystem::path& procRoot)
{
    // Format: /proc/meminfo — "Key:   value kB" per line
    // MemTotal:       16384000 kB
    // MemFree:         1234567 kB
    // MemAvailable:    8765432 kB
    // Buffers:          123456 kB
    // Cached:          4567890 kB
    // SwapTotal:       2097152 kB
    // SwapFree:        2097152 kB

    const auto meminfoPath = procRoot / "meminfo";
    const std::string pathStr = meminfoPath.string();

    constexpr std::size_t BUF_SIZE = 4096;
    std::array<char, BUF_SIZE> buf{};
    const std::size_t len = readProcFile(pathStr.c_str(), buf.data(), BUF_SIZE);
    if (len == 0)
    {
        spdlog::warn("Failed to open {}", pathStr);
        return;
    }

    const char* p = buf.data();
    const char* const end = buf.data() + len;
    constexpr uint64_t KB = 1024;

    while (p < end)
    {
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n')
        {
            ++lineEnd;
        }

        // Find the ':' separating key from value
        const char* colon = p;
        while (colon < lineEnd && *colon != ':')
        {
            ++colon;
        }
        if (colon >= lineEnd)
        {
            p = (lineEnd < end) ? lineEnd + 1 : end;
            continue;
        }

        const std::string_view key(p, static_cast<std::size_t>(colon - p));
        const char* valPtr = colon + 1;
        uint64_t value = 0;
        if (!parseNum(valPtr, lineEnd, value))
        {
            p = (lineEnd < end) ? lineEnd + 1 : end;
            continue;
        }

        if (key == "MemTotal")
        {
            counters.memory.totalBytes = value * KB;
        }
        else if (key == "MemFree")
        {
            counters.memory.freeBytes = value * KB;
        }
        else if (key == "MemAvailable")
        {
            counters.memory.availableBytes = value * KB;
        }
        else if (key == "Buffers")
        {
            counters.memory.buffersBytes = value * KB;
        }
        else if (key == "Cached")
        {
            counters.memory.cachedBytes = value * KB;
        }
        else if (key == "SwapTotal")
        {
            counters.memory.swapTotalBytes = value * KB;
        }
        else if (key == "SwapFree")
        {
            counters.memory.swapFreeBytes = value * KB;
        }

        p = (lineEnd < end) ? lineEnd + 1 : end;
    }
}

void LinuxSystemProbe::readUptime(SystemCounters& counters, const std::filesystem::path& procRoot)
{
    // Format: /proc/uptime — "uptime_seconds idle_seconds"
    // We only need the integer part of uptime_seconds.

    const std::string pathStr = (procRoot / "uptime").string();
    std::array<char, 64> buf{};
    const std::size_t len = readProcFile(pathStr.c_str(), buf.data(), buf.size());
    if (len == 0)
    {
        return;
    }

    const char* p = buf.data();
    uint64_t uptimeSec = 0;
    // from_chars on uint64_t stops at the decimal point — gives the integer part directly.
    if (parseNum(p, buf.data() + len, uptimeSec))
    {
        counters.uptimeSeconds = uptimeSec;
    }
}

void LinuxSystemProbe::readStaticInfo(SystemCounters& counters) const
{
    counters.hostname = m_Hostname;
    counters.cpuModel = m_CpuModel;
    counters.cpuCoreCount = m_NumCores;
}

void LinuxSystemProbe::readLoadAvg(SystemCounters& counters, const std::filesystem::path& procRoot)
{
    // Format: /proc/loadavg — "load1 load5 load15 running/total lastpid"

    const std::string pathStr = (procRoot / "loadavg").string();
    std::array<char, 64> buf{};
    const std::size_t len = readProcFile(pathStr.c_str(), buf.data(), buf.size());
    if (len == 0)
    {
        return;
    }

    const char* p = buf.data();
    const char* const end = buf.data() + len;
    parseDouble(p, end, counters.loadAvg1);
    parseDouble(p, end, counters.loadAvg5);
    parseDouble(p, end, counters.loadAvg15);
}

void LinuxSystemProbe::readCpuFreq(SystemCounters& counters)
{
    // Try scaling_cur_freq first; fall back to cpuinfo_cur_freq (both report kHz).
    static constexpr std::array<const char*, 2> FREQ_PATHS = {
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
        "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq",
    };

    std::array<char, 32> buf{};
    for (const char* path : FREQ_PATHS)
    {
        const std::size_t len = readProcFile(path, buf.data(), buf.size());
        if (len == 0)
        {
            continue;
        }
        const char* p = buf.data();
        uint64_t freqKHz = 0;
        if (parseNum(p, buf.data() + len, freqKHz))
        {
            counters.cpuFreqMHz = freqKHz / 1000;
            return;
        }
    }
}

void LinuxSystemProbe::readNetworkCounters(SystemCounters& counters)
{
    // Format: /proc/net/dev
    // Inter-|   Receive                                                |  Transmit
    //  face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed
    //     lo: 1234567   12345    0    0    0     0          0         0  1234567   12345    0    0    0     0       0          0
    //   eth0: 9876543   98765    0    0    0     0          0         0  5432109   54321    0    0    0     0       0          0

    const auto netDevPath = m_ProcRoot / "net" / "dev";
    const std::string pathStr = netDevPath.string();

    // Read until EOF so interfaces are not silently dropped on hosts with many veth devices.
    const std::vector<char> buf = readProcFileFull(pathStr.c_str());
    const std::size_t len = buf.size();
    if (len == 0)
    {
        spdlog::warn("Failed to open {}", pathStr);
        return;
    }

    uint64_t totalRxBytes = 0;
    uint64_t totalTxBytes = 0;

    const char* p = buf.data();
    const char* const end = buf.data() + len;

    // Skip the two header lines
    for (int skip = 0; skip < 2 && p < end; ++skip)
    {
        while (p < end && *p != '\n')
        {
            ++p;
        }
        if (p < end)
        {
            ++p;
        }
    }

    while (p < end)
    {
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n')
        {
            ++lineEnd;
        }

        // Find colon separator between interface name and stats
        const char* colon = p;
        while (colon < lineEnd && *colon != ':')
        {
            ++colon;
        }
        if (colon >= lineEnd)
        {
            p = (lineEnd < end) ? lineEnd + 1 : end;
            continue;
        }

        // Trim interface name
        const char* nameStart = p;
        while (nameStart < colon && (*nameStart == ' ' || *nameStart == '\t'))
        {
            ++nameStart;
        }
        const char* nameEnd = colon;
        while (nameEnd > nameStart && (*(nameEnd - 1) == ' ' || *(nameEnd - 1) == '\t'))
        {
            --nameEnd;
        }
        if (nameStart >= nameEnd)
        {
            p = (lineEnd < end) ? lineEnd + 1 : end;
            continue;
        }

        const std::string_view ifaceView(nameStart, static_cast<std::size_t>(nameEnd - nameStart));
        if (ifaceView == "lo")
        {
            p = (lineEnd < end) ? lineEnd + 1 : end;
            continue;
        }

        const char* q = colon + 1;
        uint64_t rxBytes = 0;
        uint64_t rxPackets = 0;
        uint64_t rxErrs = 0;
        uint64_t rxDrop = 0;
        uint64_t rxFifo = 0;
        uint64_t rxFrame = 0;
        uint64_t rxCompressed = 0;
        uint64_t rxMulticast = 0;
        uint64_t txBytes = 0;

        if (parseNum(q, lineEnd, rxBytes) && parseNum(q, lineEnd, rxPackets) && parseNum(q, lineEnd, rxErrs) &&
            parseNum(q, lineEnd, rxDrop) && parseNum(q, lineEnd, rxFifo) && parseNum(q, lineEnd, rxFrame) &&
            parseNum(q, lineEnd, rxCompressed) && parseNum(q, lineEnd, rxMulticast) && parseNum(q, lineEnd, txBytes))
        {
            totalRxBytes += rxBytes;
            totalTxBytes += txBytes;

            const std::string ifaceName(ifaceView);
            SystemCounters::InterfaceCounters ifaceCounters;
            ifaceCounters.name = ifaceName;
            ifaceCounters.displayName = ifaceName; // Linux: use system name as display name
            ifaceCounters.rxBytes = rxBytes;
            ifaceCounters.txBytes = txBytes;
            ifaceCounters.isUp = readInterfaceOperState(ifaceName);
            ifaceCounters.linkSpeedMbps = getInterfaceLinkSpeed(ifaceName, ifaceCounters.isUp);
            counters.networkInterfaces.push_back(std::move(ifaceCounters));
        }

        p = (lineEnd < end) ? lineEnd + 1 : end;
    }

    counters.netRxBytes = totalRxBytes;
    counters.netTxBytes = totalTxBytes;

    // Clean up cache entries for interfaces that no longer exist
    // (e.g., USB network adapters unplugged, VMs/containers destroyed)
    std::vector<std::string> currentInterfaces;
    currentInterfaces.reserve(counters.networkInterfaces.size());
    for (const auto& iface : counters.networkInterfaces)
    {
        currentInterfaces.push_back(iface.name);
    }
    cleanupStaleInterfaceCacheEntries(currentInterfaces);
}

void LinuxSystemProbe::cleanupStaleInterfaceCacheEntries(const std::vector<std::string>& currentInterfaces)
{
    // Remove cache entries for interfaces that are no longer present.
    // This prevents unbounded cache growth when interfaces come and go
    // (e.g., USB network adapters, VMs, containers, VPNs).
    // Convert to unordered_set for O(1) lookup instead of O(n) linear search.
    const std::unordered_set<std::string> currentSet(currentInterfaces.begin(), currentInterfaces.end());

    const std::scoped_lock lock(m_InterfaceCacheMutex);
    std::erase_if(m_InterfaceCache, [&currentSet](const auto& entry) { return !currentSet.contains(entry.first); });
}

uint64_t LinuxSystemProbe::getInterfaceLinkSpeed(const std::string& ifaceName, bool isUp)
{
    // Use cached link speed to reduce sysfs I/O.
    // Link speed rarely changes (only on cable replug or driver reload).
    // Refresh conditions:
    // 1. First access for this interface
    // 2. Interface transitioned from down to up (may have new speed after reconnection)
    // 3. TTL expired (periodic refresh every 60 seconds)

    const auto now = std::chrono::steady_clock::now();

    // Check cache under lock to determine if refresh is needed
    {
        const std::scoped_lock lock(m_InterfaceCacheMutex);

        auto it = m_InterfaceCache.find(ifaceName);
        if (it != m_InterfaceCache.end())
        {
            auto& entry = it->second;
            const bool stateTransition = (!entry.wasUp && isUp);
            const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - entry.lastSpeedCheck).count();
            const bool expired = (age >= Domain::Sampling::LINK_SPEED_CACHE_TTL_SECONDS);

            // Return cached value if still valid
            if (!stateTransition && !expired)
            {
                // Keep state tracking in sync even when we don't refresh link speed
                entry.wasUp = isUp;
                return entry.linkSpeedMbps;
            }
            // Fall through to refresh
        }
    }
    // Lock released - perform potentially blocking sysfs I/O without holding mutex

    const uint64_t newSpeed = readInterfaceLinkSpeedFromSysfs(ifaceName);

    // Update cache with new value
    // Use insert_or_assign to handle race conditions:
    // - Another thread may have inserted the entry while we were reading
    // - cleanupStaleInterfaceCacheEntries may have removed the entry
    {
        const std::scoped_lock lock(m_InterfaceCacheMutex);
        m_InterfaceCache.insert_or_assign(ifaceName,
                                          InterfaceCacheEntry{
                                              .linkSpeedMbps = newSpeed,
                                              .wasUp = isUp,
                                              .lastSpeedCheck = now,
                                          });
    }
    return newSpeed;
}

uint64_t LinuxSystemProbe::readInterfaceLinkSpeedFromSysfs(const std::string& ifaceName)
{
    // Read link speed from /sys/class/net/<iface>/speed (in Mbps).
    // Returns 0 if unavailable (e.g., virtual interfaces, down interfaces).
    // Build path in a char array to avoid string concatenation heap allocations.
    std::array<char, 128> pathBuf{};
    std::snprintf(pathBuf.data(), pathBuf.size(), "/sys/class/net/%s/speed", ifaceName.c_str());
    std::array<char, 32> buf{};
    const std::size_t len = readProcFile(pathBuf.data(), buf.data(), buf.size());
    if (len == 0)
    {
        return 0;
    }

    const char* p = buf.data();
    int64_t speedMbps = 0;
    // -1 means speed is unknown/unavailable
    if (!parseNum(p, buf.data() + len, speedMbps) || speedMbps < 0)
    {
        return 0;
    }

    // Explicit cast is safe: range check above ensures speedMbps >= 0.
    return static_cast<uint64_t>(speedMbps);
}

bool LinuxSystemProbe::readInterfaceOperState(const std::string& ifaceName)
{
    // Read operational state from /sys/class/net/<iface>/operstate.
    // Returns true only when the content is "up" (with or without trailing newline).
    // Build path in a char array to avoid string concatenation heap allocations.
    std::array<char, 128> pathBuf{};
    std::snprintf(pathBuf.data(), pathBuf.size(), "/sys/class/net/%s/operstate", ifaceName.c_str());
    std::array<char, 16> buf{};
    const std::size_t len = readProcFile(pathBuf.data(), buf.data(), buf.size());
    // "up\n" is 3 bytes; "up" is 2 — anything shorter cannot be "up".
    return (len >= 2 && buf[0] == 'u' && buf[1] == 'p');
}

} // namespace Platform

#endif
