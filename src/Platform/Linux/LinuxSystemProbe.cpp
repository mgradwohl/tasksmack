#include "LinuxSystemProbe.h"

#include <spdlog/spdlog.h>

#include <charconv>
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

    spdlog::debug("LinuxSystemProbe: {} cores, {} ticks/sec", m_NumCores, m_TicksPerSecond);
}

SystemCounters LinuxSystemProbe::read()
{
    SystemCounters counters;
    readCpuCounters(counters);
    readMemoryCounters(counters);
    readUptime(counters);
    return counters;
}

SystemCapabilities LinuxSystemProbe::capabilities() const
{
    return SystemCapabilities{.hasPerCoreCpu = true,
                              .hasMemoryAvailable = true, // Modern kernels have MemAvailable
                              .hasSwap = true,
                              .hasUptime = true,
                              .hasIoWait = true,
                              .hasSteal = true};
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

} // namespace Platform
