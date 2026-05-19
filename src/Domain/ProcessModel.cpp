#include "ProcessModel.h"

// NOLINTNEXTLINE(misc-include-cleaner) - GPUModel.h used for m_GPUModel method calls
#include "GPUModel.h"
#include "Numeric.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ProcessTypes.h"
#include "ProcessSnapshot.h"
#include "SamplingConfig.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Domain
{

ProcessModel::ProcessModel(std::unique_ptr<Platform::IProcessProbe> probe) : m_Probe(std::move(probe))
{
    // Reserve capacity upfront to avoid rehashing as processes are discovered on the
    // first refresh.  512 is comfortably above typical desktop process counts (~150-500).
    m_PerProcessState.reserve(512);

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

ProcessModel::ProcessModel(Platform::ProcessCapabilities caps, long ticksPerSec, std::uint64_t systemTotalMemory)
    : m_Capabilities(caps), m_SystemTotalMemory(systemTotalMemory), m_TicksPerSecond(ticksPerSec)
{
    // Reserve capacity upfront to avoid rehashing as processes are discovered on the
    // first refresh.  512 is comfortably above typical desktop process counts (~150-500).
    m_PerProcessState.reserve(512);

    spdlog::info("ProcessModel initialized (background-sampler mode): hasIoCounters={}, hasThreadCount={}, "
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

        // Suppress delta-based rates for implausibly short intervals. The seed call
        // in onAttach() fires just before the BackgroundSampler thread starts; the
        // thread's first callback may arrive only a few ms later (thread-startup
        // latency), making elapsedSeconds tiny and producing enormous byte/sec and
        // pageFaults/sec spikes. Any elapsed time below half the minimum configurable
        // refresh interval is treated as "no previous data" for rate purposes.
        // Note: we zero out elapsed/timeDelta here so computeSnapshot() emits zero
        // rates; we do NOT prevent the history push below (handle counts etc. don't
        // depend on elapsed time and must still be recorded every cycle).
        constexpr double MIN_ELAPSED_FOR_RATES = static_cast<double>(Sampling::REFRESH_INTERVAL_MIN_MS) / 2000.0; // half of 100ms = 0.05s
        if (elapsedSeconds < MIN_ELAPSED_FOR_RATES)
        {
            elapsedSeconds = 0.0;
            timeDeltaUs = 0;
        }
    }
    const bool hasElapsedForHistory = m_HasPrevSampleTime;
    m_PrevSampleTime = currentSampleTime;
    m_HasPrevSampleTime = true;

    std::uint64_t totalCpuDelta = 0;
    if (m_PrevTotalCpuTime > 0 && totalCpuTime > m_PrevTotalCpuTime)
    {
        totalCpuDelta = totalCpuTime - m_PrevTotalCpuTime;
    }

    // Steal capacity from the previous snapshot vector to avoid a malloc each refresh.
    // m_Snapshots is already under the write lock so this is safe.
    std::vector<ProcessSnapshot> newSnapshots;
    std::swap(newSnapshots, m_Snapshots); // steal old allocation
    newSnapshots.clear();                 // drop elements, keep capacity
    newSnapshots.reserve(counters.size());

    // Bump the generation counter once per refresh.  At the end of the loop we
    // prune any PerProcessState entry that was NOT touched this refresh (i.e.
    // its generation is still the previous value) in a single erase_if pass.
    ++m_CurrentGeneration;

    double aggNetSent = 0.0;
    double aggNetRecv = 0.0;
    double aggPageFaults = 0.0;
    double aggThreads = 0.0;
    double aggHandles = 0.0;
    double aggPower = 0.0;

    for (const auto& current : counters)
    {
        const std::uint64_t key = makeUniqueKey(current.pid, current.startTimeTicks);

        // Single map operation replaces three separate find/insert calls for
        // m_PrevCounters, m_NetworkBaselines, and m_PeakRss.
        // try_emplace returns {iterator, inserted}: inserted==true means a brand-new
        // process; inserted==false means we have state from the previous refresh.
        auto [it, inserted] = m_PerProcessState.try_emplace(key);
        PerProcessState& state = it->second;
        state.generation = m_CurrentGeneration;

        // --- previous CPU counters (for delta calculation) ---
        const Platform::ProcessCounters* previous = inserted ? nullptr : &state.counters;

        // --- network baseline ---
        // For new processes: initialise to current counters so the very first rate
        // is 0 rather than a spike from pre-existing cumulative TCP counters.
        // For existing processes: keep the original baseline untouched.
        if (inserted)
        {
            state.networkBaseline.netSentBytes = current.netSentBytes;
            state.networkBaseline.netReceivedBytes = current.netReceivedBytes;
            state.networkBaseline.firstSeenTime = currentSampleTime;
        }

        // --- peak RSS ---
        if (m_Capabilities.hasPeakRss && current.peakRssBytes > 0)
        {
            state.peakRss = current.peakRssBytes;
        }
        else
        {
            state.peakRss = inserted ? current.rssBytes : std::max(state.peakRss, current.rssBytes);
        }

        auto snapshot =
            computeSnapshot(current, previous, totalCpuDelta, m_SystemTotalMemory, m_TicksPerSecond, elapsedSeconds, timeDeltaUs);
        snapshot.peakMemoryBytes = state.peakRss;

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
        const double timeSinceFirstSeen = std::chrono::duration<double>(currentSampleTime - state.networkBaseline.firstSeenTime).count();
        if (timeSinceFirstSeen >= MIN_TIME_FOR_RATE)
        {
            // Only compute rate if current >= baseline (counter should never decrease
            // for the same process, but handle it gracefully if it does)
            if (current.netSentBytes >= state.networkBaseline.netSentBytes)
            {
                const std::uint64_t sentDelta = current.netSentBytes - state.networkBaseline.netSentBytes;
                const double rate = Numeric::toDouble(sentDelta) / timeSinceFirstSeen;
                snapshot.netSentBytesPerSec = (rate <= MAX_SANE_RATE) ? rate : 0.0;
            }
            if (current.netReceivedBytes >= state.networkBaseline.netReceivedBytes)
            {
                const std::uint64_t recvDelta = current.netReceivedBytes - state.networkBaseline.netReceivedBytes;
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
        aggHandles += static_cast<double>(snapRef.handleCount);
        aggPower += snapRef.powerWatts;

        // Store current counters so next refresh can compute deltas.
        state.counters = current;
    }

    m_Snapshots = std::move(newSnapshots);
    ++m_SnapshotVersion;

    // Merge per-process GPU data if GPUModel is available. Must happen before
    // publishing the new version so that the UI never observes a version whose
    // snapshots are still missing GPU fields.
    if (m_GPUModel != nullptr)
    {
        mergeGPUData();
    }

    // Publish only after all snapshot mutations (including GPU merge) are done.
    m_PublishedSnapshotVersion.store(m_SnapshotVersion, std::memory_order_release);

    // Prune dead processes: a single erase_if on one map instead of the previous
    // two separate erase_if calls on m_PrevCounters and m_PeakRss plus the full
    // rebuild of m_NetworkBaselines.
    std::erase_if(m_PerProcessState, [gen = m_CurrentGeneration](const auto& entry) { return entry.second.generation != gen; });

    m_PrevTotalCpuTime = totalCpuTime;

    if (hasElapsedForHistory)
    {
        // Use absolute time (since epoch) to match SystemModel's timestamp format
        const double nowSeconds = std::chrono::duration<double>(currentSampleTime.time_since_epoch()).count();
        m_Timestamps.push_back(nowSeconds);
        m_SystemNetSentHistory.push_back(aggNetSent);
        m_SystemNetRecvHistory.push_back(aggNetRecv);
        m_SystemPageFaultsHistory.push_back(aggPageFaults);
        m_SystemThreadCountHistory.push_back(aggThreads);
        m_SystemHandleCountHistory.push_back(aggHandles);
        m_SystemPowerHistory.push_back(aggPower);
        trimHistory();
    }
}

std::vector<ProcessSnapshot> ProcessModel::snapshots() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return m_Snapshots;
}

std::uint64_t ProcessModel::snapshotVersion() const
{
    return m_PublishedSnapshotVersion.load(std::memory_order_acquire);
}

bool ProcessModel::tryCopySnapshotsIfNewer(std::uint64_t lastSeenVersion,
                                           std::vector<ProcessSnapshot>& outSnapshots,
                                           std::uint64_t& outVersion) const
{
    // Fast path: avoid the shared lock on the common case where no new snapshot exists.
    // m_PublishedSnapshotVersion is always equal to m_SnapshotVersion (written together
    // under the mutex), so this atomic load is sufficient to short-circuit without locking.
    if (m_PublishedSnapshotVersion.load(std::memory_order_acquire) == lastSeenVersion)
    {
        return false;
    }

    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    if (m_SnapshotVersion == lastSeenVersion)
    {
        return false;
    }

    outSnapshots = m_Snapshots;
    outVersion = m_SnapshotVersion;
    return true;
}

std::vector<double> ProcessModel::systemNetSentHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return {m_SystemNetSentHistory.begin(), m_SystemNetSentHistory.end()};
}

std::vector<double> ProcessModel::systemNetRecvHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return {m_SystemNetRecvHistory.begin(), m_SystemNetRecvHistory.end()};
}

std::vector<double> ProcessModel::systemPageFaultsHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return {m_SystemPageFaultsHistory.begin(), m_SystemPageFaultsHistory.end()};
}

std::vector<double> ProcessModel::systemThreadCountHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return {m_SystemThreadCountHistory.begin(), m_SystemThreadCountHistory.end()};
}

std::vector<double> ProcessModel::systemHandleCountHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return {m_SystemHandleCountHistory.begin(), m_SystemHandleCountHistory.end()};
}

std::vector<double> ProcessModel::systemPowerHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return {m_SystemPowerHistory.begin(), m_SystemPowerHistory.end()};
}

std::vector<double> ProcessModel::historyTimestamps() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return {m_Timestamps.begin(), m_Timestamps.end()};
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

void ProcessModel::setGPUModel(std::shared_ptr<GPUModel> gpuModel)
{
    std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    m_GPUModel = std::move(gpuModel);
}

void ProcessModel::mergeGPUData()
{
    // Query per-process GPU counters from GPUModel
    auto gpuCounters = m_GPUModel->readProcessGPUCounters();
    if (gpuCounters.empty())
    {
        return;
    }

    // Build lookup maps: GPU ID -> friendly name
    // PDH returns LUID-based IDs (e.g., "GPU_0x00000000_0x0000F78E")
    // DXGI provides both index-based IDs ("GPU0") and LUID-based IDs
    std::unordered_map<std::string, std::string> gpuIdToName;
    auto gpuSnaps = m_GPUModel->snapshots();
    spdlog::debug("ProcessModel::mergeGPUData: Got {} GPU snapshots for name lookup", gpuSnaps.size());
    for (const auto& gpuSnap : gpuSnaps)
    {
        // Map both ID formats to the same name
        gpuIdToName[gpuSnap.gpuId] = gpuSnap.name;
        if (!gpuSnap.luidId.empty())
        {
            gpuIdToName[gpuSnap.luidId] = gpuSnap.name;
        }
        spdlog::debug("ProcessModel::mergeGPUData: GPU ID '{}' / LUID '{}' -> name '{}'", gpuSnap.gpuId, gpuSnap.luidId, gpuSnap.name);
    }

    // Build a lookup map: PID -> GPU counters (aggregated across GPUs)
    // A process may use multiple GPUs, so we aggregate
    struct AggregatedGPU
    {
        double totalUtilPercent = 0.0;
        std::uint64_t totalMemoryBytes = 0;
        double totalEncoderUtil = 0.0;
        double totalDecoderUtil = 0.0;
        std::vector<ProcessSnapshot::PerGPUUsage> perGpuBreakdown;
        std::vector<std::string> allEngines;
        std::vector<std::string> gpuNames; // Friendly names instead of UUIDs
    };
    std::unordered_map<std::int32_t, AggregatedGPU> pidToGPU;

    // Log the first counter's GPU ID for debugging
    if (!gpuCounters.empty())
    {
        spdlog::debug("ProcessModel::mergeGPUData: First per-process counter gpuId='{}'", gpuCounters[0].gpuId);
    }

    for (const auto& gc : gpuCounters)
    {
        auto& agg = pidToGPU[gc.pid];

        // Look up friendly name for this GPU
        std::string gpuName = gc.gpuId; // Default to ID if name not found
        if (auto nameIt = gpuIdToName.find(gc.gpuId); nameIt != gpuIdToName.end())
        {
            gpuName = nameIt->second;
        }

        // Add per-GPU breakdown
        ProcessSnapshot::PerGPUUsage perGpu;
        perGpu.gpuId = gc.gpuId;
        perGpu.gpuName = gpuName; // Store friendly name
        perGpu.memoryBytes = gc.gpuMemoryBytes;
        perGpu.utilPercent = gc.gpuUtilPercent;
        perGpu.engines = gc.activeEngines;
        agg.perGpuBreakdown.push_back(std::move(perGpu));

        // Aggregate totals
        agg.totalUtilPercent += gc.gpuUtilPercent;
        agg.totalMemoryBytes += gc.gpuMemoryBytes;
        agg.totalEncoderUtil += gc.encoderUtilPercent;
        agg.totalDecoderUtil += gc.decoderUtilPercent;

        // Collect unique GPU names (not IDs)
        if (std::ranges::find(agg.gpuNames, gpuName) == agg.gpuNames.end())
        {
            agg.gpuNames.push_back(gpuName);
        }

        // Collect unique engines
        for (const auto& engine : gc.activeEngines)
        {
            if (std::ranges::find(agg.allEngines, engine) == agg.allEngines.end())
            {
                agg.allEngines.push_back(engine);
            }
        }
    }

    // Merge into snapshots
    int mergedCount = 0;
    for (auto& snapshot : m_Snapshots)
    {
        auto it = pidToGPU.find(snapshot.pid);
        if (it != pidToGPU.end())
        {
            ++mergedCount;
            const auto& agg = it->second;
            // Multi-GPU utilization is summed (can exceed 100% if process uses multiple GPUs).
            // This is intentional: 150% means full utilization of 1.5 GPUs worth of compute.
            snapshot.gpuUtilPercent = agg.totalUtilPercent;
            snapshot.gpuMemoryBytes = agg.totalMemoryBytes;
            snapshot.gpuEncoderUtil = agg.totalEncoderUtil;
            snapshot.gpuDecoderUtil = agg.totalDecoderUtil;
            snapshot.gpuEngines = agg.allEngines;
            snapshot.perGpuUsage = agg.perGpuBreakdown;

            // Build comma-separated GPU device string (friendly names)
            // Pre-allocate to avoid multiple reallocations
            std::string gpuDevices;
            if (!agg.gpuNames.empty())
            {
                size_t totalLength = 0;
                for (const auto& name : agg.gpuNames)
                {
                    totalLength += name.length() + 2; // +2 for ", "
                }
                gpuDevices.reserve(totalLength);

                for (size_t i = 0; i < agg.gpuNames.size(); ++i)
                {
                    if (i > 0)
                    {
                        gpuDevices += ", ";
                    }
                    gpuDevices += agg.gpuNames[i];
                }
            }
            snapshot.gpuDevices = std::move(gpuDevices);

            // Log GPU-using processes for debugging
            spdlog::debug("ProcessModel: PID {} ({}) using GPU: {:.1f}%, {} bytes, devices='{}', engines={}",
                          snapshot.pid,
                          snapshot.name,
                          snapshot.gpuUtilPercent,
                          snapshot.gpuMemoryBytes,
                          snapshot.gpuDevices,
                          snapshot.gpuEngines.size());
        }
    }

    spdlog::debug("ProcessModel: Merged GPU data for {} processes", mergedCount);
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
    snapshot.status = current.status;                 // Pass through status from platform probe
    snapshot.publisher = current.publisher;           // Pass through publisher from platform probe
    snapshot.processType = current.processType;       // Pass through process type from platform probe
    snapshot.gdiObjectCount = current.gdiObjectCount; // Pass through GDI object count from platform probe
    snapshot.memoryBytes = current.rssBytes;
    snapshot.virtualBytes = current.virtualBytes;
    snapshot.sharedBytes = current.sharedBytes;
    snapshot.threadCount = current.threadCount;
    snapshot.handleCount = current.handleCount;
    snapshot.nice = current.nice;
    snapshot.pageFaults = current.pageFaultCount;
    snapshot.cpuAffinityMask = current.cpuAffinityMask;
    snapshot.startTimeEpoch = current.startTimeEpoch;
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
    trimFront(m_SystemHandleCountHistory);
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
