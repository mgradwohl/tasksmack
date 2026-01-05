// Keep this translation unit parseable on non-Linux platforms (e.g. Windows clangd)
// by compiling the implementation only when targeting Linux and required headers exist.
#if defined(__linux__) && __has_include(<unistd.h>)

#include "LinuxSystemProbe.h"

#include <spdlog/spdlog.h>

#include <array>
#include <concepts>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>

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

} // namespace

LinuxSystemProbe::LinuxSystemProbe()
    : m_TicksPerSecond(sysconf(_SC_CLK_TCK)), m_NumCores(checkedPositiveToSizeT(sysconf(_SC_NPROCESSORS_ONLN), 1U))
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
    std::ifstream cpuInfo("/proc/cpuinfo");
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
    readCpuCounters(counters);
    readMemoryCounters(counters);
    readUptime(counters);
    readLoadAvg(counters);
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

void LinuxSystemProbe::readCpuCounters(SystemCounters& counters)
{
    // Format: /proc/stat
    // cpu  user nice system idle iowait irq softirq steal guest guest_nice
    // cpu0 user nice system idle iowait irq softirq steal guest guest_nice
    // cpu1 ...

    std::ifstream statFile("/proc/stat");
    if (!statFile.is_open())
    {
        spdlog::warn("Failed to open /proc/stat");
        return;
    }

    std::string line;
    bool foundTotal = false;

    while (std::getline(statFile, line))
    {
        if (!line.starts_with("cpu"))
        {
            // Past CPU lines
            break;
        }

        std::istringstream iss(line);
        std::string label;
        iss >> label;

        CpuCounters cpu{};
        iss >> cpu.user >> cpu.nice >> cpu.system >> cpu.idle >> cpu.iowait >> cpu.irq >> cpu.softirq >> cpu.steal >> cpu.guest >>
            cpu.guestNice;

        if (iss.fail())
        {
            // Older kernels may not have all fields, that's OK
            // Just reset the stream and try with fewer fields
        }

        if (label == "cpu")
        {
            // Aggregate line (no number suffix)
            counters.cpuTotal = cpu;
            foundTotal = true;
        }
        else if (label.size() > 3)
        {
            // Per-core line (cpu0, cpu1, etc.)
            counters.cpuPerCore.push_back(cpu);
        }
    }

    if (!foundTotal)
    {
        spdlog::warn("Failed to parse aggregate CPU line from /proc/stat");
    }
}

void LinuxSystemProbe::readMemoryCounters(SystemCounters& counters)
{
    // Format: /proc/meminfo
    // MemTotal:       16384000 kB
    // MemFree:         1234567 kB
    // MemAvailable:    8765432 kB
    // Buffers:          123456 kB
    // Cached:          4567890 kB
    // SwapTotal:       2097152 kB
    // SwapFree:        2097152 kB
    // ...

    std::ifstream memFile("/proc/meminfo");
    if (!memFile.is_open())
    {
        spdlog::warn("Failed to open /proc/meminfo");
        return;
    }

    std::string line;
    while (std::getline(memFile, line))
    {
        std::istringstream iss(line);
        std::string key;
        uint64_t value = 0;
        std::string unit;

        iss >> key >> value >> unit;

        // Remove trailing colon from key
        if (!key.empty() && key.back() == ':')
        {
            key.pop_back();
        }

        // Convert from kB to bytes
        constexpr uint64_t KB = 1024;

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
    }
}

void LinuxSystemProbe::readUptime(SystemCounters& counters)
{
    // Format: /proc/uptime
    // uptime_seconds idle_seconds

    std::ifstream uptimeFile("/proc/uptime");
    if (!uptimeFile.is_open())
    {
        return;
    }

    double uptimeSeconds = 0.0;
    uptimeFile >> uptimeSeconds;

    if (!uptimeFile.fail())
    {
        counters.uptimeSeconds = static_cast<uint64_t>(uptimeSeconds);
    }
}

void LinuxSystemProbe::readStaticInfo(SystemCounters& counters) const
{
    counters.hostname = m_Hostname;
    counters.cpuModel = m_CpuModel;
    counters.cpuCoreCount = m_NumCores;
}

void LinuxSystemProbe::readLoadAvg(SystemCounters& counters)
{
    // Format: /proc/loadavg
    // 0.31 0.65 0.97 1/330 12345
    // load1 load5 load15 running/total lastpid

    std::ifstream loadFile("/proc/loadavg");
    if (!loadFile.is_open())
    {
        return;
    }

    loadFile >> counters.loadAvg1 >> counters.loadAvg5 >> counters.loadAvg15;
}

void LinuxSystemProbe::readCpuFreq(SystemCounters& counters)
{
    // Try to read current CPU frequency from scaling driver
    // /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq (in kHz)
    std::ifstream freqFile("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    if (freqFile.is_open())
    {
        uint64_t freqKHz = 0;
        freqFile >> freqKHz;
        if (!freqFile.fail())
        {
            counters.cpuFreqMHz = freqKHz / 1000;
            return;
        }
    }

    // Fallback: try cpuinfo_cur_freq
    std::ifstream cpuInfoFreq("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq");
    if (cpuInfoFreq.is_open())
    {
        uint64_t freqKHz = 0;
        cpuInfoFreq >> freqKHz;
        if (!cpuInfoFreq.fail())
        {
            counters.cpuFreqMHz = freqKHz / 1000;
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

    std::ifstream netFile("/proc/net/dev");
    if (!netFile.is_open())
    {
        spdlog::warn("Failed to open /proc/net/dev");
        return;
    }

    uint64_t totalRxBytes = 0;
    uint64_t totalTxBytes = 0;

    std::string line;
    // Skip first two header lines
    std::getline(netFile, line);
    std::getline(netFile, line);

    while (std::getline(netFile, line))
    {
        // Find the colon separator between interface name and stats
        auto colonPos = line.find(':');
        if (colonPos == std::string::npos)
        {
            continue;
        }

        // Extract interface name (trimmed)
        std::string iface = line.substr(0, colonPos);
        // Trim leading/trailing whitespace
        auto start = iface.find_first_not_of(" \t");
        if (start == std::string::npos)
        {
            // Interface name is all whitespace; skip this line
            continue;
        }
        auto end = iface.find_last_not_of(" \t");
        iface = iface.substr(start, (end - start) + 1);

        // Skip loopback interface - it's internal traffic
        if (iface == "lo")
        {
            continue;
        }

        // Parse the stats after the colon
        std::istringstream iss(line.substr(colonPos + 1));
        uint64_t rxBytes = 0;
        uint64_t rxPackets = 0;
        uint64_t rxErrs = 0;
        uint64_t rxDrop = 0;
        uint64_t rxFifo = 0;
        uint64_t rxFrame = 0;
        uint64_t rxCompressed = 0;
        uint64_t rxMulticast = 0;
        uint64_t txBytes = 0;

        iss >> rxBytes >> rxPackets >> rxErrs >> rxDrop >> rxFifo >> rxFrame >> rxCompressed >> rxMulticast >> txBytes;

        if (!iss.fail())
        {
            totalRxBytes += rxBytes;
            totalTxBytes += txBytes;

            // Store per-interface data
            SystemCounters::InterfaceCounters ifaceCounters;
            ifaceCounters.name = iface;
            ifaceCounters.displayName = iface; // Linux: use system name as display name
            ifaceCounters.rxBytes = rxBytes;
            ifaceCounters.txBytes = txBytes;
            ifaceCounters.isUp = readInterfaceOperState(iface);
            ifaceCounters.linkSpeedMbps = readInterfaceLinkSpeed(iface);
            counters.networkInterfaces.push_back(std::move(ifaceCounters));
        }
    }

    counters.netRxBytes = totalRxBytes;
    counters.netTxBytes = totalTxBytes;
}

uint64_t LinuxSystemProbe::readInterfaceLinkSpeed(const std::string& ifaceName)
{
    // Read link speed from /sys/class/net/<iface>/speed (in Mbps)
    // Returns 0 if unavailable (e.g., virtual interfaces, down interfaces)
    const std::string speedPath = "/sys/class/net/" + ifaceName + "/speed";
    std::ifstream speedFile(speedPath);
    if (!speedFile.is_open())
    {
        return 0;
    }

    int64_t speedMbps = 0;
    speedFile >> speedMbps;

    // -1 means speed is unknown/unavailable
    if (speedFile.fail() || speedMbps < 0)
    {
        return 0;
    }

    return static_cast<uint64_t>(speedMbps);
}

bool LinuxSystemProbe::readInterfaceOperState(const std::string& ifaceName)
{
    // Read operational state from /sys/class/net/<iface>/operstate
    // Returns true if "up", false otherwise (down, unknown, etc.)
    const std::string operstatePath = "/sys/class/net/" + ifaceName + "/operstate";
    std::ifstream operstateFile(operstatePath);
    if (!operstateFile.is_open())
    {
        return false;
    }

    std::string state;
    operstateFile >> state;

    // "up" means interface is operational
    // Other values: "down", "unknown", "lowerlayerdown", "notpresent", "dormant", "testing"
    return (state == "up");
}

} // namespace Platform

#endif
