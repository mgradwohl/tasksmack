#include "ProcessModel.h"

#include "Numeric.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>

namespace Domain
{

ProcessModel::ProcessModel(std::unique_ptr<Platform::IProcessProbe> probe) : m_Probe(std::move(probe))
{
    if (m_Probe)
    {
        m_Capabilities = m_Probe->capabilities();
        m_TicksPerSecond = m_Probe->ticksPerSecond();
        m_SystemTotalMemory = m_Probe->systemTotalMemory();
        spdlog::info("ProcessModel initialized with probe capabilities: hasIoCounters={}, hasThreadCount={}, "
                     "hasUserSystemTime={}, hasStartTime={}, hasUser={}, hasCommand={}, hasNice={}, hasPageFaults={}, "
                     "hasPeakRss={}, hasCpuAffinity={}, hasNetworkCounters={}, hasPowerUsage={}",
                     m_Capabilities.hasIoCounters,
                     m_Capabilities.hasThreadCount,
                     m_Capabilities.hasUserSystemTime,
                     m_Capabilities.hasStartTime,
                     m_Capabilities.hasUser,
                     m_Capabilities.hasCommand,
                     m_Capabilities.hasNice,
                     m_Capabilities.hasPageFaults,
                     m_Capabilities.hasPeakRss,
                     m_Capabilities.hasCpuAffinity,
                     m_Capabilities.hasNetworkCounters,
                     m_Capabilities.hasPowerUsage);
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

    const auto currentSampleTime = std::chrono::steady_clock::now();
    double elapsedSeconds = 0.0;
    std::uint64_t timeDeltaUs = 0;
    if (m_HasPrevSampleTime)
    {
        const auto delta = currentSampleTime - m_PrevSampleTime;
        elapsedSeconds = std::chrono::duration<double>(delta).count();
        timeDeltaUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(delta).count());
    }
    m_PrevSampleTime = currentSampleTime;
    m_HasPrevSampleTime = true;

    std::uint64_t totalCpuDelta = 0;
    if (m_PrevTotalCpuTime > 0 && totalCpuTime > m_PrevTotalCpuTime)
    {
        totalCpuDelta = totalCpuTime - m_PrevTotalCpuTime;
    }

    std::vector<ProcessSnapshot> newSnapshots;
    newSnapshots.reserve(counters.size());

    std::unordered_map<std::uint64_t, Platform::ProcessCounters> newPrevCounters;
    newPrevCounters.reserve(counters.size());

    std::unordered_map<std::uint64_t, std::uint64_t> newPeakRss;
    newPeakRss.reserve(counters.size());

    for (const auto& current : counters)
    {
        const std::uint64_t key = makeUniqueKey(current.pid, current.startTimeTicks);

        const Platform::ProcessCounters* previous = nullptr;
        auto prevIt = m_PrevCounters.find(key);
        if (prevIt != m_PrevCounters.end())
        {
            previous = &prevIt->second;
        }

        std::uint64_t peakRss = current.rssBytes;
        if (m_Capabilities.hasPeakRss && current.peakRssBytes > 0)
        {
            peakRss = current.peakRssBytes;
        }
        else
        {
            auto peakIt = m_PeakRss.find(key);
            if (peakIt != m_PeakRss.end())
            {
                peakRss = std::max(peakIt->second, current.rssBytes);
            }
        }
        newPeakRss[key] = peakRss;

        auto snapshot =
            computeSnapshot(current, previous, totalCpuDelta, m_SystemTotalMemory, m_TicksPerSecond, elapsedSeconds, timeDeltaUs);
        snapshot.peakMemoryBytes = peakRss;
        newSnapshots.push_back(std::move(snapshot));

        newPrevCounters[key] = current;
    }

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
                                              double elapsedSeconds,
                                              std::uint64_t timeDeltaUs) const
{
    ProcessSnapshot snapshot;
    snapshot.pid = current.pid;
    snapshot.parentPid = current.parentPid;
    snapshot.name = current.name;
    snapshot.command = current.command;
    snapshot.user = current.user;
    snapshot.displayState = translateState(current.state);
    snapshot.status = current.status; // Pass through status from platform probe
    snapshot.memoryBytes = current.rssBytes;
    snapshot.virtualBytes = current.virtualBytes;
    snapshot.sharedBytes = current.sharedBytes;
    snapshot.threadCount = current.threadCount;
    snapshot.nice = current.nice;
    snapshot.pageFaults = current.pageFaultCount;
    snapshot.cpuAffinityMask = current.cpuAffinityMask;
    snapshot.uniqueKey = makeUniqueKey(current.pid, current.startTimeTicks);

    if (systemTotalMemory > 0)
    {
        snapshot.memoryPercent = (Numeric::toDouble(current.rssBytes) / Numeric::toDouble(systemTotalMemory)) * 100.0;
    }

    if (ticksPerSecond > 0)
    {
        const std::uint64_t totalTicks = current.userTime + current.systemTime;
        snapshot.cpuTimeSeconds = Numeric::toDouble(totalTicks) / Numeric::toDouble(ticksPerSecond);
    }

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

        snapshot.netSentBytesPerSec = computeRate(current.netSentBytes, previous->netSentBytes);
        snapshot.netReceivedBytesPerSec = computeRate(current.netReceivedBytes, previous->netReceivedBytes);
        snapshot.ioReadBytesPerSec = computeRate(current.readBytes, previous->readBytes);
        snapshot.ioWriteBytesPerSec = computeRate(current.writeBytes, previous->writeBytes);
    }

    if (previous != nullptr && timeDeltaUs > 0)
    {
        if (current.energyMicrojoules >= previous->energyMicrojoules)
        {
            const std::uint64_t energyDelta = current.energyMicrojoules - previous->energyMicrojoules;
            snapshot.powerWatts = Numeric::toDouble(energyDelta) / Numeric::toDouble(timeDeltaUs);
        }
    }

    return snapshot;
}

std::uint64_t ProcessModel::makeUniqueKey(std::int32_t pid, std::uint64_t startTime)
{
    std::size_t hash = std::hash<std::int32_t>{}(pid);
    hash ^= std::hash<std::uint64_t>{}(startTime) + 0x9e3779b9U + (hash << 6) + (hash >> 2);
    return hash;
}

std::string ProcessModel::translateState(char rawState)
{
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
