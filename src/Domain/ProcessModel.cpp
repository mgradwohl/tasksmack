#include "ProcessModel.h"

#include "Numeric.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>

namespace Domain
{

ProcessModel::ProcessModel(std::unique_ptr<Platform::IProcessProbe> probe) : m_Probe(std::move(probe))
{
    if (m_Probe)
    {
        m_Capabilities = m_Probe->capabilities();
        m_TicksPerSecond = m_Probe->ticksPerSecond();
        m_SystemTotalMemory = m_Probe->systemTotalMemory();
        spdlog::info("ProcessModel initialized with probe capabilities: "
                     "hasIoCounters={}, hasThreadCount={}, hasUserSystemTime={}, hasStartTime={}, hasUser={}, "
                     "hasNetworkCounters={}, hasPeakRss={}, hasCpuAffinity={}",
                     m_Capabilities.hasIoCounters,
                     m_Capabilities.hasThreadCount,
                     m_Capabilities.hasUserSystemTime,
                     m_Capabilities.hasStartTime,
                     m_Capabilities.hasUser,
                     m_Capabilities.hasNetworkCounters,
                     m_Capabilities.hasPeakRss,
                     m_Capabilities.hasCpuAffinity);
        spdlog::debug("ProcessModel: ticksPerSecond={}, systemMemory={:.1f} GB",
                      m_TicksPerSecond,
                      Numeric::toDouble(m_SystemTotalMemory) / (1024.0 * 1024.0 * 1024.0));
    }
}

void ProcessModel::refresh()
{
    if (!m_Probe)
    {
        return;
    }

    // Read current counters
    auto currentCounters = m_Probe->enumerate();
    const std::uint64_t currentTotalCpuTime = m_Probe->totalCpuTime();

    computeSnapshots(currentCounters, currentTotalCpuTime);
}

void ProcessModel::updateFromCounters(const std::vector<Platform::ProcessCounters>& counters, std::uint64_t totalCpuTime)
{
    computeSnapshots(counters, totalCpuTime);
}

void ProcessModel::computeSnapshots(const std::vector<Platform::ProcessCounters>& counters, std::uint64_t totalCpuTime)
{
    std::unique_lock lock(m_Mutex);

    // Calculate elapsed time since last sample for rate calculations
    const auto currentSampleTime = std::chrono::steady_clock::now();
    double elapsedSeconds = 0.0;
    if (m_HasPrevSampleTime)
    {
        elapsedSeconds = std::chrono::duration<double>(currentSampleTime - m_PrevSampleTime).count();
    }
    m_PrevSampleTime = currentSampleTime;
    m_HasPrevSampleTime = true;

    // Calculate total CPU delta
    std::uint64_t totalCpuDelta = 0;
    if (m_PrevTotalCpuTime > 0 && totalCpuTime > m_PrevTotalCpuTime)
    {
        totalCpuDelta = totalCpuTime - m_PrevTotalCpuTime;
    }

    // Build new snapshots
    std::vector<ProcessSnapshot> newSnapshots;
    newSnapshots.reserve(counters.size());

    std::unordered_map<std::uint64_t, Platform::ProcessCounters> newPrevCounters;
    newPrevCounters.reserve(counters.size());

    std::unordered_map<std::uint64_t, std::uint64_t> newPeakRss;
    newPeakRss.reserve(counters.size());

    for (const auto& current : counters)
    {
        const std::uint64_t key = makeUniqueKey(current.pid, current.startTimeTicks);

        // Find previous counters for this process (if exists and same instance)
        const Platform::ProcessCounters* previous = nullptr;
        auto prevIt = m_PrevCounters.find(key);
        if (prevIt != m_PrevCounters.end())
        {
            previous = &prevIt->second;
        }


        // Track peak RSS
        std::uint64_t peakRss = current.rssBytes;
        if (m_Capabilities.hasPeakRss && current.peakRssBytes > 0)
        {
            // OS provides peak (Windows)
            peakRss = current.peakRssBytes;
        }
        else
        {
            // Track peak ourselves (Linux)
            auto peakIt = m_PeakRss.find(key);
            if (peakIt != m_PeakRss.end())
            {
                peakRss = std::max(peakIt->second, current.rssBytes);
            }
        }
        newPeakRss[key] = peakRss;

        // Compute snapshot with deltas and rates, then set peak memory
        auto snapshot = computeSnapshot(current, previous, totalCpuDelta, m_SystemTotalMemory, m_TicksPerSecond, elapsedSeconds);
        snapshot.peakMemoryBytes = peakRss;
        newSnapshots.push_back(std::move(snapshot));

        // Store for next iteration
        newPrevCounters[key] = current;
    }

    // Swap in new data
    m_Snapshots = std::move(newSnapshots);
    m_PrevCounters = std::move(newPrevCounters);
    m_PeakRss = std::move(newPeakRss);
    m_PrevTotalCpuTime = totalCpuTime;
}

std::vector<ProcessSnapshot> ProcessModel::snapshots() const
{
    std::shared_lock lock(m_Mutex);
    return m_Snapshots;
}

std::size_t ProcessModel::processCount() const
{
    std::shared_lock lock(m_Mutex);
    return m_Snapshots.size();
}

const Platform::ProcessCapabilities& ProcessModel::capabilities() const
{
    return m_Capabilities;
}

ProcessSnapshot ProcessModel::computeSnapshot(const Platform::ProcessCounters& current,
                                              const Platform::ProcessCounters* previous,
                                              std::uint64_t totalCpuDelta,
                                              std::uint64_t systemTotalMemory,
                                              long ticksPerSecond,
<<<<<<< HEAD
                                              double timeDeltaSeconds) const
=======
                                              double elapsedSeconds) const
>>>>>>> a102ce3 (Add per-process I/O rate infrastructure)
{
    ProcessSnapshot snapshot;
    snapshot.pid = current.pid;
    snapshot.parentPid = current.parentPid;
    snapshot.name = current.name;
    snapshot.command = current.command;
    snapshot.user = current.user;
    snapshot.displayState = translateState(current.state);
    snapshot.memoryBytes = current.rssBytes;
    snapshot.virtualBytes = current.virtualBytes;
    snapshot.sharedBytes = current.sharedBytes;
    snapshot.threadCount = current.threadCount;
    snapshot.nice = current.nice;
    snapshot.pageFaults = current.pageFaultCount;
    snapshot.cpuAffinityMask = current.cpuAffinityMask;
    snapshot.uniqueKey = makeUniqueKey(current.pid, current.startTimeTicks);

    // Calculate memory percentage
    if (systemTotalMemory > 0)
    {
        snapshot.memoryPercent = (Numeric::toDouble(current.rssBytes) / Numeric::toDouble(systemTotalMemory)) * 100.0;
    }

    // Calculate cumulative CPU time in seconds
    if (ticksPerSecond > 0)
    {
        const std::uint64_t totalTicks = current.userTime + current.systemTime;
        snapshot.cpuTimeSeconds = Numeric::toDouble(totalTicks) / Numeric::toDouble(ticksPerSecond);
    }

    // Compute CPU% from deltas
    // CPU% = (processCpuDelta / totalCpuDelta) * 100; totalCpuDelta already includes all cores
    if (previous != nullptr && totalCpuDelta > 0)
    {
        const std::uint64_t prevUser = previous->userTime;
        const std::uint64_t prevSystem = previous->systemTime;
        const std::uint64_t currUser = current.userTime;
        const std::uint64_t currSystem = current.systemTime;

        if (currUser >= prevUser && currSystem >= prevSystem)
        {
            const std::uint64_t userDelta = currUser - prevUser;
            const std::uint64_t systemDelta = currSystem - prevSystem;
            const std::uint64_t processDelta = userDelta + systemDelta;

            const double totalCpuDeltaD = Numeric::toDouble(totalCpuDelta);
            snapshot.cpuPercent = (Numeric::toDouble(processDelta) / totalCpuDeltaD) * 100.0;
            snapshot.cpuUserPercent = (Numeric::toDouble(userDelta) / totalCpuDeltaD) * 100.0;
            snapshot.cpuSystemPercent = (Numeric::toDouble(systemDelta) / totalCpuDeltaD) * 100.0;
        }
    }

    // Compute network and I/O rates from deltas
    if (previous != nullptr && elapsedSeconds > 0.0)
    {
        auto computeRate = [elapsedSeconds](std::uint64_t currentValue, std::uint64_t previousValue) -> double
        {
            if (currentValue >= previousValue)
            {
                const std::uint64_t delta = currentValue - previousValue;
                return Numeric::toDouble(delta) / elapsedSeconds;
            }
            return 0.0;
        };

        // Network rates
        snapshot.netSentBytesPerSec = computeRate(current.netSentBytes, previous->netSentBytes);
        snapshot.netReceivedBytesPerSec = computeRate(current.netReceivedBytes, previous->netReceivedBytes);

        // I/O rates
        snapshot.ioReadBytesPerSec = computeRate(current.readBytes, previous->readBytes);
        snapshot.ioWriteBytesPerSec = computeRate(current.writeBytes, previous->writeBytes);
    }

    return snapshot;
}

std::uint64_t ProcessModel::makeUniqueKey(std::int32_t pid, std::uint64_t startTime)
{
    // Combine PID and start time to handle PID reuse
    // Use a simple hash combining technique
    std::size_t hash = std::hash<std::int32_t>{}(pid);
    hash ^= std::hash<std::uint64_t>{}(startTime) + 0x9e3779b9U + (hash << 6) + (hash >> 2);
    return static_cast<std::uint64_t>(hash);
}

std::string ProcessModel::translateState(char rawState)
{
    // Linux /proc/[pid]/stat states:
    // R = Running, S = Sleeping, D = Disk sleep, Z = Zombie,
    // T = Stopped, t = Tracing stop, X = Dead
    switch (rawState)
    {
    case 'R':
        return "Running";
    case 'S':
        return "Sleeping";
    case 'D':
        return "Disk Sleep";
    case 'Z':
        return "Zombie";
    case 'T':
        return "Stopped";
    case 't':
        return "Tracing";
    case 'X':
        return "Dead";
    case 'I':
        return "Idle";
    default:
        return "Unknown";
    }
}

} // namespace Domain
