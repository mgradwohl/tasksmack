#include "LinuxSystemProbe.h"

#include <spdlog/spdlog.h>

#include <array>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

namespace Platform
{

LinuxSystemProbe::LinuxSystemProbe() : m_TicksPerSecond(sysconf(_SC_CLK_TCK)), m_NumCores(static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)))
{
    if (m_TicksPerSecond <= 0)
    {
        m_TicksPerSecond = 100; // Common default
        spdlog::warn("Failed to get CLK_TCK, using default: {}", m_TicksPerSecond);
    }
    if (m_NumCores <= 0)
    {
        m_NumCores = 1;
        spdlog::warn("Failed to get CPU count, using default: {}", m_NumCores);
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
                              .hasCpuFreq = true};
}

long LinuxSystemProbe::ticksPerSecond() const
{
    return m_TicksPerSecond;
}

void LinuxSystemProbe::readCpuCounters(SystemCounters& counters) const
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

void LinuxSystemProbe::readMemoryCounters(SystemCounters& counters) const
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

void LinuxSystemProbe::readUptime(SystemCounters& counters) const
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

void LinuxSystemProbe::readLoadAvg(SystemCounters& counters) const
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

void LinuxSystemProbe::readCpuFreq(SystemCounters& counters) const
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

} // namespace Platform
