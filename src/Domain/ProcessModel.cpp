#include "ProcessModel.h"

// NOLINTNEXTLINE(misc-include-cleaner) - GPUModel.h used for m_GPUModel method calls
#include "GPUModel.h"
#include "History.h"
#include "Numeric.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ProcessTypes.h"
#include "PriorityConfig.h"
#include "ProcessSnapshot.h"
#include "ProcessState.h"
#include "SamplingConfig.h"
#include "SingleLineText.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Domain
{

namespace
{

// Any elapsed time below half the minimum configurable refresh interval is treated as "no previous
// data" for rate purposes (see computeSnapshotsLocked()).
constexpr double MIN_ELAPSED_FOR_RATES = static_cast<double>(Sampling::REFRESH_INTERVAL_MIN_MS) / 2000.0;

// What a refresh's network counters allow: a rate over `seconds`, holding the last rate, or none.
struct NetworkInterval
{
    enum class Kind : std::uint8_t
    {
        Measure, // a new reading `seconds` after the previous one
        Hold,    // the probe returned the same cached reading again: keep the last rate
        Reset,   // no usable interval (too short to divide by): the rate is 0
    };
    Kind kind = Kind::Reset;
    double seconds = 0.0;
};

// When the network reads are stamped (netSampleTimeNs: by the probe, or by SocketTrafficAccumulator
// from the probe's socket readings), the interval is the time between the two reads, and an
// unchanged stamp means the probe returned its cached query again (or none) -- the only case that
// holds the last rate. Otherwise the counters were read with each refresh, so it is the
// refresh interval; one suppressed as too short (refreshElapsedSeconds 0) resets the rate to 0
// rather than republishing an old one (#1063 review).
[[nodiscard]] auto networkInterval(const Platform::ProcessCounters& current,
                                   const Platform::ProcessCounters& previous,
                                   double refreshElapsedSeconds) -> NetworkInterval
{
    if (current.netSampleTimeNs != 0 && previous.netSampleTimeNs != 0)
    {
        if (current.netSampleTimeNs == previous.netSampleTimeNs)
        {
            return {.kind = NetworkInterval::Kind::Hold};
        }
        if (current.netSampleTimeNs < previous.netSampleTimeNs)
        {
            return {};
        }
        const double seconds = Numeric::toDouble(current.netSampleTimeNs - previous.netSampleTimeNs) / 1.0e9;
        return seconds >= MIN_ELAPSED_FOR_RATES ? NetworkInterval{.kind = NetworkInterval::Kind::Measure, .seconds = seconds}
                                                : NetworkInterval{};
    }
    return refreshElapsedSeconds > 0.0 ? NetworkInterval{.kind = NetworkInterval::Kind::Measure, .seconds = refreshElapsedSeconds}
                                       : NetworkInterval{};
}

// The platform's priority class, as Domain names it (#1280). Spelled out rather than cast, so the
// two enums can't drift apart unnoticed.
[[nodiscard]] constexpr auto toPriorityClass(Platform::PriorityClass priorityClass) noexcept -> Priority::PriorityClass
{
    switch (priorityClass)
    {
    case Platform::PriorityClass::Idle:
        return Priority::PriorityClass::Idle;
    case Platform::PriorityClass::BelowNormal:
        return Priority::PriorityClass::BelowNormal;
    case Platform::PriorityClass::Normal:
        return Priority::PriorityClass::Normal;
    case Platform::PriorityClass::AboveNormal:
        return Priority::PriorityClass::AboveNormal;
    case Platform::PriorityClass::High:
        return Priority::PriorityClass::High;
    case Platform::PriorityClass::Realtime:
        return Priority::PriorityClass::Realtime;
    case Platform::PriorityClass::None:
    default:
        return Priority::PriorityClass::None;
    }
}

} // namespace

ProcessModel::ProcessModel(std::unique_ptr<Platform::IProcessProbe> probe, NowFunction now)
    : m_Probe(std::move(probe)), m_Now(std::move(now))
{
    // Reserve capacity upfront to avoid rehashing as processes are discovered on the
    // first refresh.  512 is comfortably above typical desktop process counts (~150-500).
    m_PerProcessState.reserve(512);

    applyHistoryCapacity();

    if (m_Probe)
    {
        m_Capabilities = m_Probe->capabilities();
        m_PublishedCapabilities = m_Capabilities;
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

    // One sample end to end under the sampling lock, so energy attribution (which keeps state
    // between samples) can never apply an older sample after a newer one (#1093).
    std::scoped_lock const samplingLock(m_SamplingMutex);

    const bool hadNetworkCounters = m_Probe->capabilities().hasNetworkCounters;
    auto currentCounters = m_Probe->enumerate();
    const std::uint64_t currentTotalCpuTime = m_Probe->totalCpuTime();

    // Per-process network bytes from per-connection readings: monotonic, so a connection closing or
    // being attributed late doesn't make a process's counter drop or jump (#1099). Probes that report
    // per-process network counters themselves return no reading, and theirs are used as-is.
    m_NetTraffic.apply(m_Probe->readSocketTraffic(), currentCounters);

    // The probe may turn its per-process network counters off during that read: on Windows the first
    // real EStats sample can prove them unusable (#1161). The counters enumerate() returned were
    // marked with the availability it had before, so without this the sample would publish a held or
    // zero rate as a reading for one interval, instead of unavailable (#1285).
    // Capabilities are re-read every sample, after that read, and published with this generation:
    // one withdrawn now reaches the UI rather than the startup set staying in force (#1254).
    if (Platform::ProcessCapabilities capabilities = m_Probe->capabilities(); capabilities != m_Capabilities)
    {
        spdlog::info("ProcessModel: probe capabilities changed (networkCounters={}, reducedPrivileges={})",
                     capabilities.hasNetworkCounters,
                     capabilities.hasReducedPrivileges);
        m_Capabilities = capabilities;
    }
    if (hadNetworkCounters && !m_Capabilities.hasNetworkCounters)
    {
        for (auto& counters : currentCounters)
        {
            counters.networkCountersAvailable = false;
        }
    }

    // Per-process power from a package energy counter: share each interval's energy by each
    // process's CPU time in that interval. Probes that report per-process energy themselves
    // return nullopt and their energyMicrojoules is used as-is.
    if (const auto packageEnergy = m_Probe->readPackageEnergy())
    {
        m_EnergyAttributor.attribute(currentCounters, packageEnergy->energyUj, packageEnergy->maxRangeUj, packageEnergy->busyCpuTicks);
    }

    computeSnapshotsLocked(currentCounters, currentTotalCpuTime);
}

void ProcessModel::updateFromCounters(const std::vector<Platform::ProcessCounters>& counters, std::uint64_t totalCpuTime)
{
    std::scoped_lock const samplingLock(m_SamplingMutex);
    computeSnapshotsLocked(counters, totalCpuTime);
}

void ProcessModel::computeSnapshotsLocked(const std::vector<Platform::ProcessCounters>& counters, std::uint64_t totalCpuTime)
{

    struct CachedGpuSnapshotFields
    {
        double gpuUtilPercent = 0.0;
        std::uint64_t gpuMemoryBytes = 0;
        std::uint64_t gpuDedicatedMemoryBytes = 0;
        std::uint64_t gpuSharedMemoryBytes = 0;
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
    std::size_t reserveSize = 0;

    const bool interactionActive = m_InteractionActive.load(std::memory_order_acquire);

    std::shared_ptr<const std::vector<ProcessSnapshot>> previousSnapshots;
    {
        std::shared_lock const lock(m_Mutex); // Only lock to safely read m_GPUModel and m_Snapshots
        gpuModel = m_GPUModel;
        // m_Snapshots is immutable once published, so grabbing the shared_ptr here is an O(1)
        // refcount bump -- the loop below (previously run while still holding this lock) can
        // run against the local copy after the lock is released, instead of holding readers of
        // m_Snapshots (tryCopySnapshotsIfNewer(), findSnapshot(), etc.) out for its duration.
        previousSnapshots = m_Snapshots;
    }
    reserveSize = std::max(counters.size(), previousSnapshots->size());

    if (interactionActive)
    {
        cachedGpuByUniqueKey.reserve(previousSnapshots->size());
        for (const auto& previousSnapshot : *previousSnapshots)
        {
            if ((previousSnapshot.gpuMemoryBytes == 0) && (previousSnapshot.gpuDedicatedMemoryBytes == 0) &&
                (previousSnapshot.gpuSharedMemoryBytes == 0) && (previousSnapshot.gpuUtilPercent <= 0.0) &&
                previousSnapshot.gpuDevices.empty() && previousSnapshot.perGpuUsage.empty())
            {
                continue;
            }

            cachedGpuByUniqueKey.emplace(previousSnapshot.uniqueKey,
                                         CachedGpuSnapshotFields{.gpuUtilPercent = previousSnapshot.gpuUtilPercent,
                                                                 .gpuMemoryBytes = previousSnapshot.gpuMemoryBytes,
                                                                 .gpuDedicatedMemoryBytes = previousSnapshot.gpuDedicatedMemoryBytes,
                                                                 .gpuSharedMemoryBytes = previousSnapshot.gpuSharedMemoryBytes,
                                                                 .gpuEncoderUtil = previousSnapshot.gpuEncoderUtil,
                                                                 .gpuDecoderUtil = previousSnapshot.gpuDecoderUtil,
                                                                 .gpuEngines = previousSnapshot.gpuEngines,
                                                                 .perGpuUsage = previousSnapshot.perGpuUsage,
                                                                 .gpuDevices = previousSnapshot.gpuDevices});
        }
    }

    const double maxSaneRate = m_MaxSaneNetworkRateBps.load(std::memory_order_relaxed);
    const auto currentSampleTime = m_Now();
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
    newSnapshots.reserve(reserveSize);

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
        const ProcessIdentity key{.pid = current.pid, .startTime = current.startTimeTicks};

        // Single map operation replaces separate find/insert calls for the previous
        // counters and the peak RSS.
        // try_emplace returns {iterator, inserted}: inserted==true means a brand-new
        // process; inserted==false means we have state from the previous refresh.
        auto [it, inserted] = m_PerProcessState.try_emplace(key);
        PerProcessState& state = it->second;
        state.generation = m_CurrentGeneration;

        // --- previous CPU counters (for delta calculation) ---
        const Platform::ProcessCounters* previous = inserted ? nullptr : &state.counters;

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

        // Network rates are the byte delta over the last interval (#1036). They were (bytes now -
        // bytes when first seen) / time since first seen: a lifetime average, so a burst decayed
        // over minutes and a long-watched process's line barely moved.
        //  - The interval is the one between the probe's network reads (networkInterval): a
        //    probe may cache its query across refreshes, and a delta over the refresh interval
        //    would then read 0 for the cached refreshes and several intervals' bytes for the next.
        //  - While the probe returns the same cached read, the last rate is held, not zeroed.
        //  - Both platforms report per-connection readings, so the counters are monotonic: refresh()
        //    accumulates each connection's own growth (SocketTrafficAccumulator), so a connection
        //    closing or being attributed late no longer makes them drop or jump (#1099, Windows #1256).
        //    Should a counter still drop, counterRate reports 0 for that interval rather than a
        //    wrapped or negative rate.
        //  - A rate above the sanity ceiling ([metrics] max_sane_rate_bps, 100 Gbps by default,
        //    #1123) -- e.g. a connection appearing with traffic from before it was first
        //    attributed, on Windows -- is dropped to 0 too.
        //  - Only between two readings the probe could take (#1110): with either unreadable -- another
        //    user's process without root, on Linux -- the rate is unavailable, not a 0 or a jump.
        const bool networkAvailable = current.networkCountersAvailable && (previous == nullptr || previous->networkCountersAvailable);
        const NetworkInterval netInterval =
            (previous != nullptr && networkAvailable) ? networkInterval(current, *previous, elapsedSeconds) : NetworkInterval{};
        if (netInterval.kind == NetworkInterval::Kind::Measure)
        {
            const auto netRate = [seconds = netInterval.seconds, maxSaneRate](std::uint64_t now, std::uint64_t before)
            {
                const double rate = Numeric::counterRate(now, before, seconds);
                return rate <= maxSaneRate ? rate : 0.0;
            };
            state.netSentBytesPerSec = netRate(current.netSentBytes, previous->netSentBytes);
            state.netReceivedBytesPerSec = netRate(current.netReceivedBytes, previous->netReceivedBytes);
        }
        else if (netInterval.kind == NetworkInterval::Kind::Reset)
        {
            state.netSentBytesPerSec = 0.0;
            state.netReceivedBytesPerSec = 0.0;
        }
        snapshot.netSentBytesPerSec = state.netSentBytesPerSec;
        snapshot.netReceivedBytesPerSec = state.netReceivedBytesPerSec;
        snapshot.networkAvailable = networkAvailable;

        newSnapshots.push_back(std::move(snapshot));

        // Totals are over the values that were read: an unreadable one is left out, not added as a
        // reading (#1110).
        const ProcessSnapshot& snapRef = newSnapshots.back();
        if (snapRef.networkAvailable)
        {
            aggNetSent += snapRef.netSentBytesPerSec;
            aggNetRecv += snapRef.netReceivedBytesPerSec;
        }
        aggPageFaults += snapRef.pageFaultsPerSec;
        aggThreads += static_cast<double>(snapRef.threadCount);
        if (snapRef.handleCountAvailable)
        {
            aggHandles += static_cast<double>(snapRef.handleCount);
        }
        aggPower += snapRef.powerWatts;

        // Store current counters so next refresh can compute deltas.
        state.counters = current;
    }

    // Prune dead processes: a single erase_if on one map instead of the previous
    // two separate erase_if calls on m_PrevCounters and m_PeakRss.
    std::erase_if(m_PerProcessState, [gen = m_CurrentGeneration](const auto& entry) { return entry.second.generation != gen; });

    m_PrevTotalCpuTime = totalCpuTime;

    if (gpuModel != nullptr)
    {
        // Merge immediately when idle; during interaction, throttle merges to the
        // minimum interval to keep resize/drag responsive.
        if (!interactionActive || !m_HasLastGpuMergeTime ||
            ((currentSampleTime - m_LastGpuMergeTime) >= INTERACTION_GPU_MERGE_MIN_INTERVAL))
        {
            shouldMergeGpuData = true;
        }
    }

    // GPU aggregation can be expensive (PDH queries/string work). Keep it outside
    // the ProcessModel write lock so UI readers are not blocked during resize.
    if (shouldMergeGpuData && (gpuModel != nullptr))
    {
        mergeGPUDataContained(newSnapshots, gpuModel);
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
            snapshot.gpuDedicatedMemoryBytes = cached.gpuDedicatedMemoryBytes;
            snapshot.gpuSharedMemoryBytes = cached.gpuSharedMemoryBytes;
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
                if (parentIt != pidToIndex.end() && parentIt->second != i)
                {
                    // Guard against PID reuse: pidToIndex maps parentPid to whichever *currently
                    // running* process now holds that PID, which may no longer be the process that
                    // actually forked this child if the original parent has since exited and the PID
                    // was recycled by an unrelated process between the two /proc reads that produced
                    // this batch. A real parent must have started at or before its child, so reject
                    // candidates that started later. startTimeTicks is 0 when a probe couldn't
                    // determine start time; skip the check rather than drop a legitimate link when
                    // that data isn't available.
                    const std::uint64_t candidateParentStart = counters[parentIt->second].startTimeTicks;
                    const std::uint64_t childStart = counters[i].startTimeTicks;
                    const bool startTimesKnown = candidateParentStart != 0 && childStart != 0;
                    if (!startTimesKnown || candidateParentStart <= childStart)
                    {
                        newSnapshots[parentIt->second].childrenIndices.push_back(i);
                    }
                }
            }
        }
    }

    // Build the new immutable generation before taking the lock: std::make_shared allocates
    // (control block + vector shell), and that allocator call must not happen while readers
    // are blocked on m_Mutex -- that would add allocator latency to exactly the
    // reader-blocking critical section this shared_ptr scheme exists to shrink.
    auto newSnapshotsPublication = std::make_shared<const std::vector<ProcessSnapshot>>(std::move(newSnapshots));

    // Absolute time (since the clock's epoch), to match SystemModel's timestamp format. Also the
    // watched process's sample time, so Process Details and the Overview share one timebase (#1098).
    const double sampleTimeSeconds = std::chrono::duration<double>(currentSampleTime.time_since_epoch()).count();

    // The watched process's sample of this generation, copied before taking the lock (one snapshot,
    // once per refresh -- not per UI frame, #1172). Checked again under the lock below, in case the
    // watch changed in between.
    const std::int32_t watchedPid = m_WatchedPid.load(std::memory_order_acquire);
    // MSVC STL false positive: the analyzer loses track of make_shared's control block (shared_ptr's
    // _Rep) returned by copyProcess() and reports a leak; ownership is a plain shared_ptr.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    std::shared_ptr<const ProcessSnapshot> watchedSnapshot = (watchedPid > 0) ? copyProcess(*newSnapshotsPublication, watchedPid) : nullptr;

    // Holds the outgoing generation so its destruction (freeing however many hundred
    // ProcessSnapshots' worth of strings/vectors, if this write is what drops the last
    // reference to it) happens after the lock below is released, not while it's held.
    // The same for the watched sample this publish displaces from the ring.
    std::shared_ptr<const std::vector<ProcessSnapshot>> previousGeneration;
    ProcessSample displacedSample;
    {
        std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern

        if (hasElapsedForHistory)
        {
            m_Timestamps.push(sampleTimeSeconds);
            m_SystemNetSentHistory.push(aggNetSent);
            m_SystemNetRecvHistory.push(aggNetRecv);
            m_SystemPageFaultsHistory.push(aggPageFaults);
            m_SystemThreadCountHistory.push(aggThreads);
            m_SystemHandleCountHistory.push(aggHandles);
            m_SystemPowerHistory.push(aggPower);
            trimHistory();
        }

        previousGeneration = std::move(m_Snapshots);      // move out, not destroy -- ownership transfers to the local
        m_Snapshots = std::move(newSnapshotsPublication); // pointer swap only, no allocation or destruction
        ++m_SnapshotVersion;
        ++m_SystemHistoryVersion;
        m_PublishedCapabilities = m_Capabilities;
        m_SnapshotSampleTimeSeconds = sampleTimeSeconds;

        // Every generation published while a process is watched gets a sample, the process absent
        // from it included, so a reader can tell "exited" and "missed generations" apart (#1098).
        if (const std::int32_t pidNow = m_WatchedPid.load(std::memory_order_relaxed); pidNow > 0)
        {
            if (pidNow != watchedPid)
            {
                // watchProcess() ran between the copy above and this lock: rare, so copy again here.
                watchedSnapshot = copyProcess(*m_Snapshots, pidNow);
            }
            displacedSample = pushWatchedSampleLocked(ProcessSample{
                .snapshot = std::move(watchedSnapshot), .version = m_SnapshotVersion, .sampleTimeSeconds = sampleTimeSeconds});
        }

        m_PublishedSnapshotVersion.store(m_SnapshotVersion, std::memory_order_release);
        m_PublishedSystemHistoryVersion.store(m_SystemHistoryVersion, std::memory_order_release);
        if (shouldMergeGpuData)
        {
            m_LastGpuMergeTime = m_Now();
            m_HasLastGpuMergeTime = true;
        }
    }
}

std::vector<ProcessSnapshot> ProcessModel::snapshots() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return *m_Snapshots;
}

std::optional<ProcessSnapshot> ProcessModel::findSnapshot(std::int32_t pid) const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    for (const auto& snap : *m_Snapshots)
    {
        if (snap.pid == pid)
        {
            return snap;
        }
    }
    return std::nullopt;
}

std::optional<ProcessModel::SnapshotLookupResult> ProcessModel::findSnapshotWithVersion(std::int32_t pid) const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    for (const auto& snap : *m_Snapshots)
    {
        if (snap.pid == pid)
        {
            // Read under the same lock as the scan above, so this can never observe a version
            // from a different publish than the snapshot it's paired with: the writer (refresh()/
            // updateFromCounters()) updates m_Snapshots and m_PublishedSnapshotVersion together
            // under its own exclusive lock on this same mutex.
            return SnapshotLookupResult{.snapshot = snap, .version = m_PublishedSnapshotVersion.load(std::memory_order_acquire)};
        }
    }
    return std::nullopt;
}

std::shared_ptr<const ProcessSnapshot> ProcessModel::copyProcess(const std::vector<ProcessSnapshot>& snapshots, std::int32_t pid)
{
    const auto it = std::ranges::find(snapshots, pid, &ProcessSnapshot::pid);
    return (it != snapshots.end()) ? std::make_shared<const ProcessSnapshot>(*it) : nullptr;
}

ProcessSample ProcessModel::pushWatchedSampleLocked(ProcessSample sample)
{
    ProcessSample displaced;
    if (m_WatchedSampleCount < m_WatchedSamples.size())
    {
        m_WatchedSamples[(m_WatchedSampleStart + m_WatchedSampleCount) % m_WatchedSamples.size()] = std::move(sample);
        ++m_WatchedSampleCount;
    }
    else
    {
        // Full: the oldest slot becomes the newest.
        displaced = std::exchange(m_WatchedSamples[m_WatchedSampleStart], std::move(sample));
        m_WatchedSampleStart = (m_WatchedSampleStart + 1) % m_WatchedSamples.size();
    }
    return displaced;
}

void ProcessModel::watchProcess(std::int32_t pid)
{
    const std::int32_t watched = std::max(pid, 0);

    // Start the new watch with an empty ring, and note the current generation for its first sample.
    // The replacement ring is allocated here, before the lock, and the old one (with the previous
    // watch's samples) is destroyed after it is released.
    std::vector<ProcessSample> ring(WATCHED_SAMPLE_CAPACITY);
    std::shared_ptr<const std::vector<ProcessSnapshot>> current;
    std::uint64_t currentVersion = 0;
    double currentSampleTime = 0.0;
    {
        std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
        m_WatchedPid.store(watched, std::memory_order_release);
        m_WatchedSamples.swap(ring);
        m_WatchedSampleStart = 0;
        m_WatchedSampleCount = 0;
        current = m_Snapshots;
        currentVersion = m_SnapshotVersion;
        currentSampleTime = m_SnapshotSampleTimeSeconds;
    }
    if (watched == 0 || currentVersion == 0)
    {
        return; // Not watching, or nothing published yet: the first refresh records the first sample
    }

    // The process as the current generation lists it, copied outside the lock. A publish landing
    // in between has already recorded a newer sample, which supersedes this one.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks) - MSVC STL false positive, see computeSnapshotsLocked()
    auto seed = copyProcess(*current, watched);
    std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    if (m_WatchedPid.load(std::memory_order_relaxed) == watched && m_WatchedSampleCount == 0)
    {
        static_cast<void>(pushWatchedSampleLocked(
            ProcessSample{.snapshot = std::move(seed), .version = currentVersion, .sampleTimeSeconds = currentSampleTime}));
    }
}

bool ProcessModel::watchedSamplesSince(std::uint64_t lastSeenVersion, std::vector<ProcessSample>& outSamples) const
{
    // Fast path, the common case of a frame with no new generation: one atomic load, no lock, no copy.
    if (m_PublishedSnapshotVersion.load(std::memory_order_acquire) == lastSeenVersion)
    {
        return false;
    }

    outSamples.reserve(outSamples.size() + WATCHED_SAMPLE_CAPACITY); // no allocation under the lock
    const std::size_t before = outSamples.size();
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    for (std::size_t i = 0; i < m_WatchedSampleCount; ++i)
    {
        const ProcessSample& sample = m_WatchedSamples[(m_WatchedSampleStart + i) % m_WatchedSamples.size()];
        if (sample.version > lastSeenVersion)
        {
            outSamples.push_back(sample); // shared_ptr copy: a refcount bump
        }
    }
    return outSamples.size() > before;
}

std::uint64_t ProcessModel::snapshotVersion() const
{
    return m_PublishedSnapshotVersion.load(std::memory_order_acquire);
}

bool ProcessModel::tryCopySystemHistoriesIfNewer(std::uint64_t lastSeenVersion, ProcessSystemHistories& outHistories) const
{
    if (m_PublishedSystemHistoryVersion.load(std::memory_order_acquire) == lastSeenVersion)
    {
        return false;
    }

    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    if (m_SystemHistoryVersion == lastSeenVersion)
    {
        return false;
    }

    outHistories.version = m_SystemHistoryVersion;
    outHistories.timestamps = HistoryUtils::toVector(m_Timestamps);
    outHistories.power = HistoryUtils::toVector(m_SystemPowerHistory);
    outHistories.pageFaults = HistoryUtils::toVector(m_SystemPageFaultsHistory);
    outHistories.threadCount = HistoryUtils::toVector(m_SystemThreadCountHistory);
    outHistories.handleCount = HistoryUtils::toVector(m_SystemHandleCountHistory);
    outHistories.capabilities = m_PublishedCapabilities;
    return true;
}

bool ProcessModel::tryCopySnapshotsIfNewer(std::uint64_t lastSeenVersion,
                                           std::shared_ptr<const std::vector<ProcessSnapshot>>& outSnapshots,
                                           std::uint64_t& outVersion,
                                           Platform::ProcessCapabilities* outCapabilities) const
{
    // Fast path: avoid the shared lock on the common case where no new snapshot exists.
    // m_PublishedSnapshotVersion is always equal to m_SnapshotVersion (written together
    // under the mutex), so this atomic load is sufficient to short-circuit without locking.
    if (m_PublishedSnapshotVersion.load(std::memory_order_acquire) == lastSeenVersion)
    {
        return false;
    }

    // Capture into locals under the lock rather than assigning directly into outSnapshots:
    // outSnapshots is typically the caller's long-lived cache (e.g. ProcessesPanel's render
    // cache), which is often the last owner of the *previous* generation by the time a newer
    // one is fetched. Assigning straight into it here would destroy that previous
    // generation -- freeing however many hundred ProcessSnapshots' worth of strings/vectors
    // -- while still holding the shared lock, continuing to block the writer's next unique_lock
    // for the deep-copy-equivalent cost this shared_ptr scheme exists to avoid.
    std::shared_ptr<const std::vector<ProcessSnapshot>> newSnapshots;
    std::uint64_t newVersion = 0;
    {
        std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
        if (m_SnapshotVersion == lastSeenVersion)
        {
            return false;
        }
        newSnapshots = m_Snapshots; // refcount bump only -- newSnapshots is a fresh local, nothing to destroy
        newVersion = m_SnapshotVersion;
        if (outCapabilities != nullptr)
        {
            *outCapabilities = m_PublishedCapabilities;
        }
    }

    // The caller's previous generation, if this assignment drops its last reference, is
    // destroyed here -- after the lock above has already been released.
    outSnapshots = std::move(newSnapshots);
    outVersion = newVersion;
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
    // The same guardrail as the other models, so every chart covers the same window (#1145).
    m_MaxHistorySeconds = Sampling::clampHistorySeconds(seconds);
    applyHistoryCapacity();
    trimHistory();
    // The trimmed histories are a new generation of them: without this, tryCopySystemHistoriesIfNewer()
    // kept handing out the old window's data until the next sample (#1145). The snapshot version is
    // left alone -- the process list did not change, and every snapshot generation has a watched sample.
    if (m_SystemHistoryVersion != 0)
    {
        ++m_SystemHistoryVersion;
        m_PublishedSystemHistoryVersion.store(m_SystemHistoryVersion, std::memory_order_release);
    }
}

void ProcessModel::setMaxSaneNetworkRate(double bytesPerSecond) noexcept
{
    m_MaxSaneNetworkRateBps.store(Sampling::clampMaxSaneRateBps(bytesPerSecond), std::memory_order_relaxed);
}

std::size_t ProcessModel::processCount() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return m_Snapshots->size();
}

Platform::ProcessCapabilities ProcessModel::capabilities() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return m_PublishedCapabilities;
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

    // Build lookup map: GPU ID -> what the per-process breakdown shows about that adapter.
    // PDH returns LUID-based IDs (e.g., "GPU_0x00000000_0x0000F78E")
    // DXGI provides both index-based IDs ("GPU0") and LUID-based IDs
    //
    // The integrated flag travels with the name. It used to be left out, so PerGPUUsage kept its
    // default of false and the process details pane labelled every adapter "Discrete", including
    // one the system GPU tab correctly called integrated (#963).
    struct GpuIdentity
    {
        std::string name;
        bool isIntegrated = false;
        bool memoryIsShared = false;
    };
    std::unordered_map<std::string, GpuIdentity> gpuIdToIdentity;
    auto gpuSnaps = gpuModel->snapshots();
    for (const auto& gpuSnap : gpuSnaps)
    {
        // Map both ID formats to the same adapter
        const GpuIdentity identity{.name = gpuSnap.name, .isIntegrated = gpuSnap.isIntegrated, .memoryIsShared = gpuSnap.memoryIsShared};
        gpuIdToIdentity[gpuSnap.gpuId] = identity;
        if (!gpuSnap.luidId.empty())
        {
            gpuIdToIdentity[gpuSnap.luidId] = identity;
        }
    }

    // Build a lookup map: PID -> GPU counters (aggregated across GPUs)
    // A process may use multiple GPUs, so we aggregate
    // One rule with the adapter figures beside them on the GPU tab (#1164): utilization is the
    // busiest GPU's (an adapter's is 0-100; a sum passed 100% while Process Details clamped it), and
    // memory counts, per GPU, the segment that GPU's "used" figure counts, as the platform says
    // (GPUSnapshot::memoryIsShared: shared on a Windows integrated GPU, dedicated elsewhere) -- never
    // inferred from the reading, so a 0 shared reading stays a shared 0 -- and a process never shows
    // more than its adapters use. The dedicated and shared amounts are kept apart as well.
    struct AggregatedGPU
    {
        double maxUtilPercent = 0.0;
        std::uint64_t totalMemoryBytes = 0;
        std::uint64_t totalDedicatedMemoryBytes = 0;
        std::uint64_t totalSharedMemoryBytes = 0;
        double maxEncoderUtil = 0.0;
        double maxDecoderUtil = 0.0;
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
        bool isIntegrated = false;      // Unknown adapter: keep the default rather than guess
        bool memoryIsShared = false;
        if (const auto identityIt = gpuIdToIdentity.find(gc.gpuId); identityIt != gpuIdToIdentity.end())
        {
            gpuName = identityIt->second.name;
            isIntegrated = identityIt->second.isIntegrated;
            memoryIsShared = identityIt->second.memoryIsShared;
        }

        // Add per-GPU breakdown
        ProcessSnapshot::PerGPUUsage perGpu;
        perGpu.gpuId = gc.gpuId;
        perGpu.gpuName = gpuName; // Store friendly name
        perGpu.isIntegrated = isIntegrated;
        perGpu.dedicatedMemoryBytes = gc.gpuMemoryBytes;
        perGpu.sharedMemoryBytes = gc.gpuSharedMemoryBytes;
        // Where the platform has no shared segment (Linux: NVML, ROCm SMI) the adapter's used figure
        // is its dedicated memory -- an APU's carve-out included -- so that is what counts. The
        // choice follows the adapter's segment, not the value: shared usage crossing 0 on a Windows
        // iGPU doesn't switch "GPU memory" to dedicated and back.
        perGpu.memoryBytes = memoryIsShared ? gc.gpuSharedMemoryBytes : gc.gpuMemoryBytes;
        perGpu.utilPercent = Numeric::clampPercent(gc.gpuUtilPercent);
        perGpu.engines = gc.activeEngines;

        // Aggregate across GPUs
        agg.maxUtilPercent = std::max(agg.maxUtilPercent, perGpu.utilPercent);
        agg.totalMemoryBytes += perGpu.memoryBytes;
        agg.totalDedicatedMemoryBytes += gc.gpuMemoryBytes;
        agg.totalSharedMemoryBytes += gc.gpuSharedMemoryBytes;
        agg.maxEncoderUtil = std::max(agg.maxEncoderUtil, Numeric::clampPercent(gc.encoderUtilPercent));
        agg.maxDecoderUtil = std::max(agg.maxDecoderUtil, Numeric::clampPercent(gc.decoderUtilPercent));
        agg.perGpuBreakdown.push_back(std::move(perGpu));

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
            snapshot.gpuUtilPercent = agg.maxUtilPercent;
            snapshot.gpuMemoryBytes = agg.totalMemoryBytes;
            snapshot.gpuDedicatedMemoryBytes = agg.totalDedicatedMemoryBytes;
            snapshot.gpuSharedMemoryBytes = agg.totalSharedMemoryBytes;
            snapshot.gpuEncoderUtil = agg.maxEncoderUtil;
            snapshot.gpuDecoderUtil = agg.maxDecoderUtil;
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

void ProcessModel::mergeGPUDataContained(std::vector<ProcessSnapshot>& snapshots, const std::shared_ptr<GPUModel>& gpuModel)
{
    // Uncontained, a throw here (bad_alloc, a DRM parse error, a PDH wrapper) escaped refresh()
    // after the per-process state had already advanced, so a probe that threw every time stopped
    // the process list from ever updating again (#1142). The processes are published regardless,
    // without GPU fields for this refresh.
    try
    {
        mergeGPUData(snapshots, gpuModel);
        if (m_GpuMergeFailing)
        {
            spdlog::info("ProcessModel: per-process GPU data is being merged again");
            m_GpuMergeFailing = false;
        }
    }
    catch (const std::exception& ex)
    {
        if (!m_GpuMergeFailing)
        {
            spdlog::warn("ProcessModel: merging per-process GPU data failed; publishing processes without it: {}", ex.what());
            m_GpuMergeFailing = true;
        }
        // A merge that threw part way may have filled some processes and not others: clear them all.
        for (auto& snapshot : snapshots)
        {
            snapshot.gpuUtilPercent = 0.0;
            snapshot.gpuMemoryBytes = 0;
            snapshot.gpuDedicatedMemoryBytes = 0;
            snapshot.gpuSharedMemoryBytes = 0;
            snapshot.gpuEncoderUtil = 0.0;
            snapshot.gpuDecoderUtil = 0.0;
            snapshot.gpuEngines.clear();
            snapshot.perGpuUsage.clear();
            snapshot.gpuDevices.clear();
        }
    }
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
    // Sanitized here rather than in each platform probe: both /proc/[pid]/cmdline (which uses NUL
    // only as the argument *separator*) and the Windows PEB command line can carry a newline inside
    // an argument, and a process controls its own argv. See SingleLineText.h for why that breaks the
    // process table (#919).
    snapshot.name = toSingleLine(current.name);
    snapshot.command = toSingleLine(current.command);
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
    snapshot.handleCount = current.handleCountAvailable ? current.handleCount : 0;
    snapshot.handleCountAvailable = current.handleCountAvailable;
    // A rate needs both of the readings it is taken between (#1110): from an unreadable 0 to a real
    // count would read as the process's whole lifetime of I/O in one interval.
    snapshot.ioAvailable = current.ioCountersAvailable && (previous == nullptr || previous->ioCountersAvailable);
    snapshot.nice = current.nice;
    snapshot.priorityClass = toPriorityClass(current.priorityClass);
    snapshot.pageFaults = current.pageFaultCount;
    snapshot.cpuAffinityMask = current.cpuAffinityMask;
    snapshot.startTimeEpoch = current.startTimeEpoch;
    snapshot.startTimeTicks = current.startTimeTicks;
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
        // This works correctly for I/O because the probes report per-process cumulative
        // transfer counters (Linux: /proc/[pid]/io; Windows: the SystemProcessInformation
        // snapshot) that are stable and monotonically increasing.
        if (snapshot.ioAvailable)
        {
            snapshot.ioReadBytesPerSec = Numeric::counterRate(current.readBytes, previous->readBytes, elapsedSeconds);
            snapshot.ioWriteBytesPerSec = Numeric::counterRate(current.writeBytes, previous->writeBytes, elapsedSeconds);
        }
        snapshot.pageFaultsPerSec = Numeric::counterRate(current.pageFaultCount, previous->pageFaultCount, elapsedSeconds);
        // Network rates are computed in computeSnapshotsLocked(), which has the per-process state they
        // need (networkInterval, held rates).
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

    // Drop entries older than the configured time window, except the newest of them while a newer
    // sample remains (see HistoryUtils::discardBefore, #1016). All rings are pushed in lockstep with
    // m_Timestamps, so a single discard count keeps them aligned. discardFront is O(1): no copies,
    // rebuilds, or allocations. With maxHistorySeconds == 0 the cutoff equals the newest timestamp,
    // so only the current sample is retained (no anchor before a zero-length window).
    const double cutoff = m_Timestamps.latest() - m_MaxHistorySeconds;
    static_cast<void>(HistoryUtils::discardBefore(m_Timestamps,
                                                  cutoff,
                                                  m_SystemNetSentHistory,
                                                  m_SystemNetRecvHistory,
                                                  m_SystemPageFaultsHistory,
                                                  m_SystemThreadCountHistory,
                                                  m_SystemHandleCountHistory,
                                                  m_SystemPowerHistory));
}

void ProcessModel::applyHistoryCapacity()
{
    // Size every ring so the configured time window fits even at the fastest
    // supported refresh cadence; time-based trimming governs actual retention.
    const std::size_t capacity = Sampling::historyCapacityForSeconds(m_MaxHistorySeconds);
    m_Timestamps.setCapacity(capacity);
    m_SystemNetSentHistory.setCapacity(capacity);
    m_SystemNetRecvHistory.setCapacity(capacity);
    m_SystemPageFaultsHistory.setCapacity(capacity);
    m_SystemThreadCountHistory.setCapacity(capacity);
    m_SystemHandleCountHistory.setCapacity(capacity);
    m_SystemPowerHistory.setCapacity(capacity);
}

std::string ProcessModel::translateState(char rawState)
{
    return std::string(processStateName(rawState));
}

} // namespace Domain
