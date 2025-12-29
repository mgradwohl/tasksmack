#include "ProcessModel.h"

#include "Numeric.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

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
    std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern

    const auto currentSampleTime = std::chrono::steady_clock::now();
    if (!m_HasStartTime)
    {
        m_StartTime = currentSampleTime;
        m_HasStartTime = true;
    }
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

    // Track active keys to prune stale entries (reuse existing set)
    m_ActiveKeys.clear();
    m_ActiveKeys.reserve(counters.size());

    // Carry forward network baselines for existing processes, add new ones
    std::unordered_map<std::uint64_t, NetworkBaseline> newNetworkBaselines;
    newNetworkBaselines.reserve(counters.size());

    double aggNetSent = 0.0;
    double aggNetRecv = 0.0;
    double aggPageFaults = 0.0;
    double aggThreads = 0.0;
    double aggPower = 0.0;

    for (const auto& current : counters)
    {
        const std::uint64_t key = makeUniqueKey(current.pid, current.startTimeTicks);
        m_ActiveKeys.insert(key);

        const Platform::ProcessCounters* previous = nullptr;
        auto prevIt = m_PrevCounters.find(key);
        if (prevIt != m_PrevCounters.end())
        {
            previous = &prevIt->second;
        }

        // Track network baseline for this process (see ProcessModel.h for rationale)
        // The baseline approach computes average rate since first seen, avoiding
        // wild spikes from TCP connection churn in Windows EStats.
        NetworkBaseline baseline;
        auto baselineIt = m_NetworkBaselines.find(key);
        if (baselineIt != m_NetworkBaselines.end())
        {
            // Existing process - keep the original baseline so we compute
            // rate = (current - original_baseline) / time_since_first_seen
            baseline = baselineIt->second;
        }
        else
        {
            // New process - record current counters as baseline
            // This absorbs any pre-existing cumulative values from TCP connections
            // that were open before we started monitoring this process
            baseline.netSentBytes = current.netSentBytes;
            baseline.netReceivedBytes = current.netReceivedBytes;
            baseline.firstSeenTime = currentSampleTime;
        }
        newNetworkBaselines[key] = baseline;

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
        m_PeakRss[key] = peakRss;

        auto snapshot =
            computeSnapshot(current, previous, totalCpuDelta, m_SystemTotalMemory, m_TicksPerSecond, elapsedSeconds, timeDeltaUs);
        snapshot.peakMemoryBytes = peakRss;

        // =======================================================================
        // Network Rate Calculation (Baseline Approach)
        // =======================================================================
        // Formula: rate = (currentCounters - baselineCounters) / timeSinceFirstSeen
        //
        // This gives us average bytes/sec since we started monitoring this process.
        // See ProcessModel.h for detailed explanation of why we use this approach
        // instead of delta-based rates (TL;DR: Windows TCP EStats connection churn).
        //
        // We require a minimum time elapsed (0.5s) before computing rates to avoid
        // division by tiny time values on first sample causing huge rate spikes.
        // We also apply a sanity check ceiling (100 Gbps) as a safety net.
        //
        // Note: A more accurate approach (ETW, eBPF, etc.) could enable instantaneous
        // rate calculation similar to I/O rates below. See ProcessModel.h for details.
        // =======================================================================
        constexpr double MIN_TIME_FOR_RATE = 0.5;          // seconds
        constexpr double MAX_SANE_RATE = 12'500'000'000.0; // 100 Gbps in bytes/sec
        const double timeSinceFirstSeen = std::chrono::duration<double>(currentSampleTime - baseline.firstSeenTime).count();
        if (timeSinceFirstSeen >= MIN_TIME_FOR_RATE)
        {
            // Only compute rate if current >= baseline (counter should never decrease
            // for the same process, but handle it gracefully if it does)
            if (current.netSentBytes >= baseline.netSentBytes)
            {
                const std::uint64_t sentDelta = current.netSentBytes - baseline.netSentBytes;
                const double rate = Numeric::toDouble(sentDelta) / timeSinceFirstSeen;
                snapshot.netSentBytesPerSec = (rate <= MAX_SANE_RATE) ? rate : 0.0;
            }
            if (current.netReceivedBytes >= baseline.netReceivedBytes)
            {
                const std::uint64_t recvDelta = current.netReceivedBytes - baseline.netReceivedBytes;
                const double rate = Numeric::toDouble(recvDelta) / timeSinceFirstSeen;
                snapshot.netReceivedBytesPerSec = (rate <= MAX_SANE_RATE) ? rate : 0.0;
            }
        }

        newSnapshots.push_back(std::move(snapshot));

        const ProcessSnapshot& snapRef = newSnapshots.back();
        aggNetSent += snapRef.netSentBytesPerSec;
        aggNetRecv += snapRef.netReceivedBytesPerSec;
        aggPageFaults += snapRef.pageFaultsPerSec;
        aggThreads += static_cast<double>(snapRef.threadCount);
        aggPower += snapRef.powerWatts;

        m_PrevCounters[key] = current;
    }

    m_Snapshots = std::move(newSnapshots);
    m_NetworkBaselines = std::move(newNetworkBaselines);

    // Prune stale entries from tracking maps (dead processes) using modern C++23 idiom
    std::erase_if(m_PrevCounters, [this](const auto& entry) { return !m_ActiveKeys.contains(entry.first); });
    std::erase_if(m_PeakRss, [this](const auto& entry) { return !m_ActiveKeys.contains(entry.first); });

    m_PrevTotalCpuTime = totalCpuTime;

    if (m_HasPrevSampleTime && elapsedSeconds > 0.0)
    {
        // Use absolute time (since epoch) to match SystemModel's timestamp format
        const double nowSeconds = std::chrono::duration<double>(currentSampleTime.time_since_epoch()).count();
        m_Timestamps.push_back(nowSeconds);
        m_SystemNetSentHistory.push_back(aggNetSent);
        m_SystemNetRecvHistory.push_back(aggNetRecv);
        m_SystemPageFaultsHistory.push_back(aggPageFaults);
        m_SystemThreadCountHistory.push_back(aggThreads);
        m_SystemPowerHistory.push_back(aggPower);
        trimHistory();
    }
}

std::vector<ProcessSnapshot> ProcessModel::snapshots() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return m_Snapshots;
}

std::vector<double> ProcessModel::systemNetSentHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return std::vector<double>(m_SystemNetSentHistory.begin(), m_SystemNetSentHistory.end());
}

std::vector<double> ProcessModel::systemNetRecvHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return std::vector<double>(m_SystemNetRecvHistory.begin(), m_SystemNetRecvHistory.end());
}

std::vector<double> ProcessModel::systemPageFaultsHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return std::vector<double>(m_SystemPageFaultsHistory.begin(), m_SystemPageFaultsHistory.end());
}

std::vector<double> ProcessModel::systemThreadCountHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return std::vector<double>(m_SystemThreadCountHistory.begin(), m_SystemThreadCountHistory.end());
}

std::vector<double> ProcessModel::systemPowerHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return std::vector<double>(m_SystemPowerHistory.begin(), m_SystemPowerHistory.end());
}

std::vector<double> ProcessModel::historyTimestamps() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return std::vector<double>(m_Timestamps.begin(), m_Timestamps.end());
}

void ProcessModel::setMaxHistorySeconds(double seconds)
{
    std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    m_MaxHistorySeconds = std::max(0.0, seconds);
    trimHistory();
}

std::size_t ProcessModel::processCount() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
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
                                              std::uint64_t timeDeltaUs)
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

        // I/O and page fault rates use delta-based calculation:
        //   rate = (currentCounter - previousCounter) / elapsedSeconds
        //
        // This works correctly for I/O because GetProcessIoCounters returns per-process
        // cumulative counters that are stable and monotonically increasing (not affected
        // by file handle churn the way network counters are affected by TCP connection churn).
        //
        // Network rates are computed separately in computeSnapshots() using the baseline
        // approach - see comments there and in ProcessModel.h for details.
        snapshot.ioReadBytesPerSec = computeRate(current.readBytes, previous->readBytes);
        snapshot.ioWriteBytesPerSec = computeRate(current.writeBytes, previous->writeBytes);
        snapshot.pageFaultsPerSec = computeRate(current.pageFaultCount, previous->pageFaultCount);
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

void ProcessModel::trimHistory()
{
    if (m_Timestamps.empty())
    {
        return;
    }

    const double cutoff = m_Timestamps.back() - m_MaxHistorySeconds;

    // Find how many timestamps are older than cutoff
    size_t trimCount = 0;
    for (const auto& ts : m_Timestamps)
    {
        if (ts < cutoff)
        {
            ++trimCount;
        }
        else
        {
            break; // Timestamps are in order, so we can stop early
        }
    }

    // Trim the same count from all history deques to keep them synchronized
    auto trimFront = [trimCount](auto& dq)
    {
        for (size_t i = 0; i < trimCount && !dq.empty(); ++i)
        {
            dq.pop_front();
        }
    };

    trimFront(m_Timestamps);
    trimFront(m_SystemNetSentHistory);
    trimFront(m_SystemNetRecvHistory);
    trimFront(m_SystemPageFaultsHistory);
    trimFront(m_SystemThreadCountHistory);
    trimFront(m_SystemPowerHistory);
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
