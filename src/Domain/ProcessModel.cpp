#include "ProcessModel.h"

// NOLINTNEXTLINE(misc-include-cleaner) - GPUModel.h used for m_GPUModel method calls
#include "GPUModel.h"
#include "History.h"
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
    struct CachedGpuSnapshotFields
    {
        double gpuUtilPercent = 0.0;
        std::uint64_t gpuMemoryBytes = 0;
        double gpuEncoderUtil = 0.0;
        double gpuDecoderUtil = 0.0;
        std::vector<std::string> gpuEngines;
        std::vector<ProcessSnapshot::PerGPUUsage> perGpuUsage;
        std::string gpuDevices;
    };

    constexpr auto INTERACTION_GPU_MERGE_MIN_INTERVAL = std::chrono::milliseconds(1500);

    std::vector<ProcessSnapshot> newSnapshots;
    std::unordered_map<std::uint64_t, CachedGpuSnapshotFields> cachedGpuByUniqueKey;
    std::shared_ptr<GPUModel> gpuModel;
    bool shouldMergeGpuData = false;

    {
        std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern

        const auto currentSampleTime = std::chrono::steady_clock::now();
        const bool interactionActive = m_InteractionActive.load(std::memory_order_acquire);
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
            constexpr double MIN_ELAPSED_FOR_RATES = static_cast<double>(Sampling::REFRESH_INTERVAL_MIN_MS) / 2000.0;
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

        // Keep m_Snapshots published and readable until the replacement snapshot vector
        // is fully prepared and ready to publish.
        newSnapshots.reserve(std::max(counters.size(), m_Snapshots.size()));
        if (interactionActive)
        {
            cachedGpuByUniqueKey.reserve(m_Snapshots.size());
            for (const auto& previousSnapshot : m_Snapshots)
            {
                if ((previousSnapshot.gpuMemoryBytes == 0) && (previousSnapshot.gpuUtilPercent <= 0.0) &&
                    previousSnapshot.gpuDevices.empty() && previousSnapshot.perGpuUsage.empty())
                {
                    continue;
                }

                cachedGpuByUniqueKey.emplace(previousSnapshot.uniqueKey,
                                             CachedGpuSnapshotFields{previousSnapshot.gpuUtilPercent,
                                                                     previousSnapshot.gpuMemoryBytes,
                                                                     previousSnapshot.gpuEncoderUtil,
                                                                     previousSnapshot.gpuDecoderUtil,
                                                                     previousSnapshot.gpuEngines,
                                                                     previousSnapshot.perGpuUsage,
                                                                     previousSnapshot.gpuDevices});
            }
        }
        newSnapshots.clear();

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
            const double timeSinceFirstSeen =
                std::chrono::duration<double>(currentSampleTime - state.networkBaseline.firstSeenTime).count();
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

        // Prune dead processes: a single erase_if on one map instead of the previous
        // two separate erase_if calls on m_PrevCounters and m_PeakRss plus the full
        // rebuild of m_NetworkBaselines.
        std::erase_if(m_PerProcessState, [gen = m_CurrentGeneration](const auto& entry) { return entry.second.generation != gen; });

        m_PrevTotalCpuTime = totalCpuTime;
        gpuModel = m_GPUModel;
        if (gpuModel != nullptr)
        {
            if (!interactionActive)
            {
                shouldMergeGpuData = true;
            }
            else if (!m_HasLastGpuMergeTime || ((currentSampleTime - m_LastGpuMergeTime) >= INTERACTION_GPU_MERGE_MIN_INTERVAL))
            {
                shouldMergeGpuData = true;
            }
        }

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

    // GPU aggregation can be expensive (PDH queries/string work). Keep it outside
    // the ProcessModel write lock so UI readers are not blocked during resize.
    if (shouldMergeGpuData && (gpuModel != nullptr))
    {
        mergeGPUData(newSnapshots, gpuModel);
    }
    else if (!cachedGpuByUniqueKey.empty())
    {
        for (auto& snapshot : newSnapshots)
        {
            const auto it = cachedGpuByUniqueKey.find(snapshot.uniqueKey);
            if (it == cachedGpuByUniqueKey.end())
            {
                continue;
            }
            const CachedGpuSnapshotFields& cached = it->second;
            snapshot.gpuUtilPercent = cached.gpuUtilPercent;
            snapshot.gpuMemoryBytes = cached.gpuMemoryBytes;
            snapshot.gpuEncoderUtil = cached.gpuEncoderUtil;
            snapshot.gpuDecoderUtil = cached.gpuDecoderUtil;
            snapshot.gpuEngines = cached.gpuEngines;
            snapshot.perGpuUsage = cached.perGpuUsage;
            snapshot.gpuDevices = cached.gpuDevices;
        }
    }

    // --- Build Process Tree Hierarchy ---
    {
        std::unordered_map<std::int32_t, std::size_t> pidToIndex;
        pidToIndex.reserve(newSnapshots.size());
        for (std::size_t i = 0; i < newSnapshots.size(); ++i)
        {
            pidToIndex[newSnapshots[i].pid] = i;
        }

        for (std::size_t i = 0; i < newSnapshots.size(); ++i)
        {
            const std::int32_t parentPid = newSnapshots[i].parentPid;
            if (parentPid > 0)
            {
                auto parentIt = pidToIndex.find(parentPid);
                if (parentIt != pidToIndex.end())
                {
                    newSnapshots[parentIt->second].childrenIndices.push_back(i);
                }
            }
        }
    }

    {
        std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
        m_Snapshots = std::move(newSnapshots);
        ++m_SnapshotVersion;
        m_PublishedSnapshotVersion.store(m_SnapshotVersion, std::memory_order_release);
        if (shouldMergeGpuData)
        {
            m_LastGpuMergeTime = std::chrono::steady_clock::now();
            m_HasLastGpuMergeTime = true;
        }
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
    return HistoryUtils::toVector(m_SystemNetSentHistory);
}

std::vector<double> ProcessModel::systemNetRecvHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_SystemNetRecvHistory);
}

std::vector<double> ProcessModel::systemPageFaultsHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_SystemPageFaultsHistory);
}

std::vector<double> ProcessModel::systemThreadCountHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_SystemThreadCountHistory);
}

std::vector<double> ProcessModel::systemHandleCountHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_SystemHandleCountHistory);
}

std::vector<double> ProcessModel::systemPowerHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_SystemPowerHistory);
}

std::vector<double> ProcessModel::historyTimestamps() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_Timestamps);
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

void ProcessModel::setInteractionActive(const bool active) noexcept
{
    m_InteractionActive.store(active, std::memory_order_release);
}

void ProcessModel::mergeGPUData(std::vector<ProcessSnapshot>& snapshots, const std::shared_ptr<GPUModel>& gpuModel)
{
    if (gpuModel == nullptr)
    {
        return;
    }

    // Query per-process GPU counters from GPUModel
    auto gpuCounters = gpuModel->readProcessGPUCounters();
    if (gpuCounters.empty())
    {
        return;
    }

    // Build lookup maps: GPU ID -> friendly name
    // PDH returns LUID-based IDs (e.g., "GPU_0x00000000_0x0000F78E")
    // DXGI provides both index-based IDs ("GPU0") and LUID-based IDs
    std::unordered_map<std::string, std::string> gpuIdToName;
    auto gpuSnaps = gpuModel->snapshots();
    for (const auto& gpuSnap : gpuSnaps)
    {
        // Map both ID formats to the same name
        gpuIdToName[gpuSnap.gpuId] = gpuSnap.name;
        if (!gpuSnap.luidId.empty())
        {
            gpuIdToName[gpuSnap.luidId] = gpuSnap.name;
        }
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
    for (auto& snapshot : snapshots)
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
        }
    }

    spdlog::debug("ProcessModel::mergeGPUData: merged GPU data for {} processes", mergedCount);
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
        // I/O and page fault rates use delta-based calculation:
        //   rate = (currentCounter - previousCounter) / elapsedSeconds
        //
        // This works correctly for I/O because GetProcessIoCounters returns per-process
        // cumulative counters that are stable and monotonically increasing (not affected
        // by file handle churn the way network counters are affected by TCP connection churn).
        //
        // Network rates are computed separately in computeSnapshots() using the baseline
        // approach - see comments there and in ProcessModel.h for details.
        snapshot.ioReadBytesPerSec = Numeric::counterRate(current.readBytes, previous->readBytes, elapsedSeconds);
        snapshot.ioWriteBytesPerSec = Numeric::counterRate(current.writeBytes, previous->writeBytes, elapsedSeconds);
        snapshot.pageFaultsPerSec = Numeric::counterRate(current.pageFaultCount, previous->pageFaultCount, elapsedSeconds);
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

    static_cast<void>(HistoryUtils::trimBefore(m_Timestamps,
                                               cutoff,
                                               m_SystemNetSentHistory,
                                               m_SystemNetRecvHistory,
                                               m_SystemPageFaultsHistory,
                                               m_SystemThreadCountHistory,
                                               m_SystemHandleCountHistory,
                                               m_SystemPowerHistory));
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
