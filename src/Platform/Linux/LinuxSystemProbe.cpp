// Keep this translation unit parseable on non-Linux platforms (e.g. Windows clangd)
// by compiling the implementation only when targeting Linux and required headers exist.
#if defined(__linux__) && __has_include(<unistd.h>)

#include "LinuxSystemProbe.h"

#include "Domain/SamplingConfig.h"
#include "LinuxCpuDetails.h"
#include "Platform/CpuDetails.h"
#include "Platform/SystemTypes.h"
#include "ProcParsing.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
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
    : LinuxSystemProbe(std::move(procRoot), std::filesystem::path("/sys/class/net"))
{}

LinuxSystemProbe::LinuxSystemProbe(std::filesystem::path procRoot, std::filesystem::path sysClassNetRoot)
    : LinuxSystemProbe(std::move(procRoot), std::move(sysClassNetRoot), std::filesystem::path("/sys/devices/system/cpu"))
{}

LinuxSystemProbe::LinuxSystemProbe(std::filesystem::path procRoot,
                                   std::filesystem::path sysClassNetRoot,
                                   std::filesystem::path cpuSysfsRoot)
    : m_ProcRoot(std::move(procRoot)),
      m_SysClassNetRoot(std::move(sysClassNetRoot)),
      m_CpuSysfsRoot(std::move(cpuSysfsRoot)),
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

    // The details describe the CPUs /proc/cpuinfo lists now, so a CPU that goes online or offline
    // before the first read() is a change from this set, not the baseline (#809).
    m_CpuDetails.details = LinuxCpuDetails::read(m_ProcRoot, m_CpuSysfsRoot, &m_CpuDetails.ids);
    m_LastCoreCount = m_NumCores;

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
    readCpuDetails(counters);
    readMemoryCounters(counters, m_ProcRoot);
    readUptime(counters, m_ProcRoot);
    readLoadAvg(counters, m_ProcRoot);
    readCpuFreq(counters, m_CpuSysfsRoot);
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
                              .hasNetworkCounters = true,
                              .hasVirtualizationInfo = false}; // No virtualization/VBS facts on Linux (#809)
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

        CpuCounters cpu{};
        if (!isTotal)
        {
            // Keep the N of "cpuN" as the core's identity. The kernel lists online CPUs only, so
            // with cpu2 offline the lines run cpu0, cpu1, cpu3: position is not identity (#1229).
            const auto [idEnd, ec] = std::from_chars(q, lineEnd, cpu.coreId);
            if (ec != std::errc{} || (idEnd < lineEnd && *idEnd != ' ' && *idEnd != '\t'))
            {
                spdlog::debug("Skipping unparseable per-core line in {}", pathStr);
                p = (lineEnd < end) ? lineEnd + 1 : end;
                continue;
            }
        }

        // Skip past the label token to reach the first numeric field
        while (q < lineEnd && *q != ' ' && *q != '\t')
        {
            ++q;
        }

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
            counters.memory.hasAvailableBytes = true;
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
}

void LinuxSystemProbe::readCpuDetails(SystemCounters& counters)
{
    const auto sampledIds = counters.cpuPerCore | std::views::transform(&CpuCounters::coreId);
    const std::scoped_lock lock(m_CpuDetailsMutex);
    if (CpuTopology::cpuDetailsNeedRead(m_CpuDetails, sampledIds))
    {
        // Rare: only when the set of online CPUs changes, never every sample
        std::vector<std::size_t> describedIds;
        CpuDetails fresh = LinuxCpuDetails::read(m_ProcRoot, m_CpuSysfsRoot, &describedIds);
        (void) CpuTopology::commitIfConsistent(sampledIds, std::move(describedIds), std::move(fresh), m_CpuDetails);
    }
    if (!counters.cpuPerCore.empty())
    {
        m_LastCoreCount = counters.cpuPerCore.size();
    }
    counters.cpuCoreCount = m_LastCoreCount;
    counters.cpuDetails = m_CpuDetails.details;
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

void LinuxSystemProbe::readCpuFreq(SystemCounters& counters, const std::filesystem::path& cpuSysfsRoot)
{
    // Try scaling_cur_freq first; fall back to cpuinfo_cur_freq (both report kHz).
    static constexpr std::array<const char*, 2> FREQ_FILES = {"scaling_cur_freq", "cpuinfo_cur_freq"};
    const std::filesystem::path cpufreqDir = cpuSysfsRoot / "cpu0" / "cpufreq";

    std::array<char, 32> buf{};
    for (const char* file : FREQ_FILES)
    {
        const std::string path = (cpufreqDir / file).string();
        const std::size_t len = readProcFile(path.c_str(), buf.data(), buf.size());
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
            const std::string ifaceName(ifaceView);
            SystemCounters::InterfaceCounters ifaceCounters;
            ifaceCounters.name = ifaceName;
            ifaceCounters.displayName = ifaceName; // Linux: use system name as display name
            ifaceCounters.rxBytes = rxBytes;
            ifaceCounters.txBytes = txBytes;
            ifaceCounters.isUp = readInterfaceOperState(m_SysClassNetRoot, ifaceName);
            ifaceCounters.linkSpeedMbps = getInterfaceLinkSpeed(ifaceName, ifaceCounters.isUp);
            counters.networkInterfaces.push_back(std::move(ifaceCounters));
        }

        p = (lineEnd < end) ? lineEnd + 1 : end;
    }

    std::vector<std::string> currentInterfaces;
    currentInterfaces.reserve(counters.networkInterfaces.size());
    for (const auto& iface : counters.networkInterfaces)
    {
        currentInterfaces.push_back(iface.name);
    }
    std::vector<std::optional<bool>> isVirtual;
    const bool interfaceSetChanged = classifyInterfaces(currentInterfaces, isVirtual);
    for (std::size_t i = 0; i < counters.networkInterfaces.size(); ++i)
    {
        counters.networkInterfaces[i].isVirtual = isVirtual[i].value_or(false);
        counters.networkInterfaces[i].isVirtualKnown = isVirtual[i].has_value();
    }

    // The totals count hardware interfaces only: traffic over a bridge, veth, VPN tunnel or VLAN
    // also crosses a hardware interface, so counting both doubled it (#1106). With no hardware
    // interface at all (inside a container, whose eth0 is a veth) every interface counts, so the
    // Total isn't 0. Keep in step with SystemModel's Total, which applies the same rule to rates.
    const bool anyHardware = std::ranges::any_of(counters.networkInterfaces, [](const auto& iface) { return !iface.isVirtual; });
    uint64_t totalRxBytes = 0;
    uint64_t totalTxBytes = 0;
    for (const auto& iface : counters.networkInterfaces)
    {
        if (!anyHardware || !iface.isVirtual)
        {
            totalRxBytes += iface.rxBytes;
            totalTxBytes += iface.txBytes;
        }
    }
    counters.netRxBytes = totalRxBytes;
    counters.netTxBytes = totalTxBytes;

    // Clean up cache entries for interfaces that no longer exist (e.g., USB network adapters
    // unplugged, VMs/containers destroyed). Entries are only ever added for listed interfaces, so
    // there can be stale ones only after the interface set changed.
    if (interfaceSetChanged)
    {
        cleanupStaleInterfaceCacheEntries(currentInterfaces);
    }
}

bool LinuxSystemProbe::classifyInterfaces(const std::vector<std::string>& names, std::vector<std::optional<bool>>& isVirtual)
{
    isVirtual.assign(names.size(), std::nullopt);
    std::vector<std::size_t> toLookUp;
    bool setChanged = false;
    {
        const std::scoped_lock lock(m_InterfaceCacheMutex);
        setChanged = (names != m_ClassifiedInterfaces);
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            const auto cached = setChanged ? m_InterfaceIsVirtual.end() : m_InterfaceIsVirtual.find(names[i]);
            if (cached != m_InterfaceIsVirtual.end())
            {
                isVirtual[i] = cached->second;
            }
            else
            {
                toLookUp.push_back(i);
            }
        }
    }
    if (toLookUp.empty() && !setChanged)
    {
        return false;
    }

    // The sysfs lookups run without the lock, like the link-speed reads.
    for (const std::size_t i : toLookUp)
    {
        isVirtual[i] = isVirtualInterface(m_SysClassNetRoot, names[i]);
    }

    // Build the cache's entries first and commit its key (the interface set) last, so a cache
    // that says it is valid for a set always holds entries looked up for that set.
    const std::scoped_lock lock(m_InterfaceCacheMutex);
    if (setChanged)
    {
        std::unordered_map<std::string, bool> rebuilt;
        rebuilt.reserve(names.size());
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            if (const std::optional<bool> known = isVirtual[i]; known.has_value())
            {
                rebuilt.insert_or_assign(names[i], *known);
            }
        }
        // Copy the key before touching either member, then commit both with non-throwing swaps: a
        // throw while copying can no longer leave the old key paired with the new entries.
        auto key = names;
        m_InterfaceIsVirtual.swap(rebuilt);
        m_ClassifiedInterfaces.swap(key);
    }
    else if (names == m_ClassifiedInterfaces) // unless another read() changed the set meanwhile
    {
        for (const std::size_t i : toLookUp)
        {
            if (const std::optional<bool> known = isVirtual[i]; known.has_value())
            {
                m_InterfaceIsVirtual.insert_or_assign(names[i], *known);
            }
        }
    }
    return setChanged;
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

    const uint64_t newSpeed = readInterfaceLinkSpeedFromSysfs(m_SysClassNetRoot, ifaceName);

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

uint64_t LinuxSystemProbe::readInterfaceLinkSpeedFromSysfs(const std::filesystem::path& sysClassNetRoot, std::string_view ifaceName)
{
    // Read link speed from <sysClassNetRoot>/<iface>/speed (in Mbps).
    // Returns 0 if unavailable (e.g., virtual interfaces, down interfaces).
    // Read only on a cache miss (getInterfaceLinkSpeed), so building the path costs nothing per sample.
    const std::string path = (sysClassNetRoot / ifaceName / "speed").string();
    std::array<char, 32> buf{};
    const std::size_t len = readProcFile(path.c_str(), buf.data(), buf.size());
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

std::optional<bool> LinuxSystemProbe::isVirtualInterface(const std::filesystem::path& sysClassNetRoot, std::string_view ifaceName)
{
    // A hardware NIC (PCI, USB, SDIO, Hyper-V netvsc, virtio) has a `device` link to its bus device;
    // a software interface doesn't. When the interface can't be found at all (sysfs not mounted, or
    // it vanished) it can't be classified: the caller counts it as hardware, the pre-#1106 behavior,
    // and the UI falls back to its name (#1260).
    // Anything that stops the lookup short of an answer -- a permission or I/O error -- is "can't
    // tell" too, never "virtual": exists() returns false for those as well, so ec is checked.
    std::error_code ec;
    const auto ifaceDir = sysClassNetRoot / ifaceName;
    if (!std::filesystem::exists(ifaceDir, ec) || ec)
    {
        return std::nullopt;
    }
    // The link itself, not its target: a hardware NIC's `device` link counts even if what it points
    // at can't be resolved.
    const auto deviceLink = std::filesystem::symlink_status(ifaceDir / "device", ec);
    if (deviceLink.type() == std::filesystem::file_type::not_found)
    {
        return true;
    }
    if (ec)
    {
        return std::nullopt;
    }
    return false;
}

bool LinuxSystemProbe::readInterfaceOperState(const std::filesystem::path& sysClassNetRoot, std::string_view ifaceName)
{
    // Read operational state from <sysClassNetRoot>/<iface>/operstate.
    // Returns true only when the content is "up" (with or without trailing newline).
    const std::string path = (sysClassNetRoot / ifaceName / "operstate").string();
    std::array<char, 16> buf{};
    const std::size_t len = readProcFile(path.c_str(), buf.data(), buf.size());
    // "up\n" is 3 bytes; "up" is 2 — anything shorter cannot be "up".
    return (len >= 2 && buf[0] == 'u' && buf[1] == 'p');
}

} // namespace Platform

#endif
