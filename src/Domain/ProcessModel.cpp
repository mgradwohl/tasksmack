#include "ProcessModel.h"

#include "Numeric.h"

#include <spdlog/spdlog.h>

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
                     "hasIoCounters={}, hasThreadCount={}, hasUserSystemTime={}, hasStartTime={}, hasUser={}",
                     m_Capabilities.hasIoCounters,
                     m_Capabilities.hasThreadCount,
                     m_Capabilities.hasUserSystemTime,
                     m_Capabilities.hasStartTime,
                     m_Capabilities.hasUser);
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

        // Compute snapshot with deltas
        newSnapshots.push_back(computeSnapshot(current, previous, totalCpuDelta, m_SystemTotalMemory, m_TicksPerSecond));

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
                                              long ticksPerSecond) const
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
