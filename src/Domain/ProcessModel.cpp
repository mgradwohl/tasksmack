#include "ProcessModel.h"

#include <spdlog/spdlog.h>

#include <functional>

namespace Domain
{

ProcessModel::ProcessModel(std::unique_ptr<Platform::IProcessProbe> probe) : m_Probe(std::move(probe))
{
    if (m_Probe)
    {
        m_Capabilities = m_Probe->capabilities();
        spdlog::info("ProcessModel initialized with probe capabilities: "
                     "hasIoCounters={}, hasThreadCount={}, hasUserSystemTime={}, hasStartTime={}",
                     m_Capabilities.hasIoCounters,
                     m_Capabilities.hasThreadCount,
                     m_Capabilities.hasUserSystemTime,
                     m_Capabilities.hasStartTime);
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
    uint64_t currentTotalCpuTime = m_Probe->totalCpuTime();

    computeSnapshots(currentCounters, currentTotalCpuTime);
}

void ProcessModel::updateFromCounters(const std::vector<Platform::ProcessCounters>& counters, uint64_t totalCpuTime)
{
    computeSnapshots(counters, totalCpuTime);
}

void ProcessModel::computeSnapshots(const std::vector<Platform::ProcessCounters>& counters, uint64_t totalCpuTime)
{
    std::unique_lock lock(m_Mutex);

    // Calculate total CPU delta
    uint64_t totalCpuDelta = 0;
    if (m_PrevTotalCpuTime > 0 && totalCpuTime > m_PrevTotalCpuTime)
    {
        totalCpuDelta = totalCpuTime - m_PrevTotalCpuTime;
    }

    // Build new snapshots
    std::vector<ProcessSnapshot> newSnapshots;
    newSnapshots.reserve(counters.size());

    std::unordered_map<uint64_t, Platform::ProcessCounters> newPrevCounters;
    newPrevCounters.reserve(counters.size());

    for (const auto& current : counters)
    {
        uint64_t key = makeUniqueKey(current.pid, current.startTimeTicks);

        // Find previous counters for this process (if exists and same instance)
        const Platform::ProcessCounters* previous = nullptr;
        auto prevIt = m_PrevCounters.find(key);
        if (prevIt != m_PrevCounters.end())
        {
            previous = &prevIt->second;
        }

        // Compute snapshot with deltas
        newSnapshots.push_back(computeSnapshot(current, previous, totalCpuDelta));

        // Store for next iteration
        newPrevCounters[key] = current;
    }

    // Swap in new data
    m_Snapshots = std::move(newSnapshots);
    m_PrevCounters = std::move(newPrevCounters);
    m_PrevTotalCpuTime = totalCpuTime;
}

std::vector<ProcessSnapshot> ProcessModel::snapshots() const
{
    std::shared_lock lock(m_Mutex);
    return m_Snapshots;
}

size_t ProcessModel::processCount() const
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
                                              uint64_t totalCpuDelta) const
{
    ProcessSnapshot snapshot;
    snapshot.pid = current.pid;
    snapshot.parentPid = current.parentPid;
    snapshot.name = current.name;
    snapshot.displayState = translateState(current.state);
    snapshot.memoryBytes = current.rssBytes;
    snapshot.virtualBytes = current.virtualBytes;
    snapshot.threadCount = current.threadCount;
    snapshot.uniqueKey = makeUniqueKey(current.pid, current.startTimeTicks);

    // Compute CPU% from deltas
    // CPU% = (processCpuDelta / totalCpuDelta) * 100 * numCores
    // Note: totalCpuDelta already accounts for all cores
    if (previous != nullptr && totalCpuDelta > 0)
    {
        uint64_t prevProcessTime = previous->userTime + previous->systemTime;
        uint64_t currProcessTime = current.userTime + current.systemTime;

        if (currProcessTime >= prevProcessTime)
        {
            uint64_t processDelta = currProcessTime - prevProcessTime;
            snapshot.cpuPercent = (static_cast<double>(processDelta) / static_cast<double>(totalCpuDelta)) * 100.0;
        }
    }

    // Compute I/O rates (bytes per second) if available
    // Note: This would need time delta; for now just store raw values
    // TODO: Add proper I/O rate calculation with time delta

    return snapshot;
}

uint64_t ProcessModel::makeUniqueKey(int32_t pid, uint64_t startTime)
{
    // Combine PID and start time to handle PID reuse
    // Use a simple hash combining technique
    size_t hash = std::hash<int32_t>{}(pid);
    hash ^= std::hash<uint64_t>{}(startTime) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
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
