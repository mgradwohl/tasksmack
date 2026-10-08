#include "GPUModel.h"

#include "GPUSnapshot.h"
#include "History.h"
#include "Numeric.h"
#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"
#include "PublicationSlot.h"
#include "SamplingConfig.h"
#include "SharedHistory.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
// NOLINTNEXTLINE(misc-include-cleaner) - std::ranges::find_if and std::ranges::find are in <ranges>
#include <ranges>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Domain
{

namespace
{

/// A history value as a float: NaN for a placeholder recorded while the GPU was missing (#1146).
template<typename T> [[nodiscard]] float sampleOrNaN(const GPUSnapshot& sample, T value)
{
    return sample.sampled ? static_cast<float>(value) : std::numeric_limits<float>::quiet_NaN();
}

/// A field's history value: NaN for a placeholder or a sample whose read of that field failed (#1111).
template<typename T> [[nodiscard]] float readingOrNaN(const GPUSnapshot& sample, T value, bool available)
{
    return available ? sampleOrNaN(sample, value) : std::numeric_limits<float>::quiet_NaN();
}

/// A GPU's utilization from its DRM clients' cumulative engine busyness (#1267). Per engine class,
/// each client in both samples adds the share of the interval it kept the class busy (its busy
/// change over its total's change, both in one unit); the shares summed over the class's capacity
/// (engine count) give the class's percent, and the busiest class is the GPU's. A client seen in only
/// one sample (started or exited in between), or whose counters went backwards, adds nothing; no
/// clients at all is an idle GPU, 0%.
[[nodiscard]] double engineUtilizationPercent(std::span<const Platform::GPUEngineClientCounters> current,
                                              std::span<const Platform::GPUEngineClientCounters> previous)
{
    double busiest = 0.0;
    for (std::size_t engineClass = 0; engineClass < Platform::GPU_ENGINE_CLASS_COUNT; ++engineClass)
    {
        double busyShare = 0.0;
        std::uint32_t capacity = 1;
        for (const auto& client : current)
        {
            const auto before = std::ranges::find(previous, client.clientId, &Platform::GPUEngineClientCounters::clientId);
            if (before == previous.end())
            {
                continue;
            }
            const auto& now = client.engines.at(engineClass);
            const auto& then = before->engines.at(engineClass);
            if (!now.available || !then.available || now.total <= then.total || now.busy < then.busy)
            {
                continue;
            }
            busyShare += Numeric::toDouble(now.busy - then.busy) / Numeric::toDouble(now.total - then.total);
            capacity = std::max(capacity, now.capacity);
        }
        busiest = std::max(busiest, (busyShare / static_cast<double>(capacity)) * 100.0);
    }
    // Clients' reads are moments apart, so the shares of a fully busy class can sum just past 100%.
    return std::clamp(busiest, 0.0, 100.0);
}

/// Keeps each DRM client's engine busy counter at its high-water mark in the counters stored as the
/// next sample's baseline. The DRM usage-stats contract lets drm-engine-* / drm-cycles-* go briefly
/// backwards and asks userspace to keep the previous, larger value until the counter catches up; a
/// lower baseline would make the next rise, still below the old value, count as real busy time
/// (#1350 review).
void carryBusyHighWater(Platform::GPUCounters& current, const Platform::GPUCounters& previous)
{
    for (auto& client : current.engineClients)
    {
        const auto before = std::ranges::find(previous.engineClients, client.clientId, &Platform::GPUEngineClientCounters::clientId);
        if (before == previous.engineClients.end())
        {
            continue;
        }
        for (std::size_t engineClass = 0; engineClass < Platform::GPU_ENGINE_CLASS_COUNT; ++engineClass)
        {
            auto& now = client.engines.at(engineClass);
            const auto& then = before->engines.at(engineClass);
            if (now.available && then.available && now.busy < then.busy)
            {
                now.busy = then.busy;
            }
        }
    }
}

/// The GPU clock: NaN when the read failed or returned 0 MHz. A 0 is the probes' "couldn't read it"
/// (DRM, and NVML on a suspended GPU), and the Clock NowBar already shows N/A for it (#995), so the
/// line has a gap there too rather than diving to 0 (#1111).
[[nodiscard]] float gpuClockOrNaN(const GPUSnapshot& sample)
{
    return readingOrNaN(sample, sample.gpuClockMHz, sample.gpuClockAvailable && sample.gpuClockMHz > 0);
}

/// The fan speed as a float: NaN when it couldn't be read, not 0.0F, so the chart shows a gap
/// rather than a flat "0%" indistinguishable from an idle fan.
[[nodiscard]] float fanSpeedOrNaN(const GPUSnapshot& sample)
{
    return sample.fanSpeedAvailable ? sampleOrNaN(sample, sample.fanSpeedPercent) : std::numeric_limits<float>::quiet_NaN();
}

} // namespace

GPUModel::HistorySample GPUModel::historySample(const GPUSnapshot& sample) noexcept
{
    return HistorySample{
        .timestamp = sample.captureTimeSec,
        // An unread memory sample keeps no bytes: a 0 total is the "no byte figures" marker, so the
        // tooltip shows N/A rather than a placeholder "0 / <total>" (#1111).
        .memoryUsedBytes = sample.memoryAvailable ? sample.memoryUsedBytes : 0,
        .memoryTotalBytes = sample.memoryAvailable ? sample.memoryTotalBytes : 0,
        .utilization = readingOrNaN(sample, sample.utilizationPercent, sample.utilizationAvailable),
        .memoryPercent = readingOrNaN(sample, sample.memoryUsedPercent, sample.memoryAvailable),
        .gpuClock = gpuClockOrNaN(sample),
        .encoder = sampleOrNaN(sample, sample.encoderUtilPercent),
        .decoder = sampleOrNaN(sample, sample.decoderUtilPercent),
        .temperature = readingOrNaN(sample, sample.temperatureC, sample.temperatureAvailable),
        .power = readingOrNaN(sample, sample.powerDrawWatts, sample.powerAvailable),
        .fanSpeed = fanSpeedOrNaN(sample),
        .sampled = sample.sampled,
    };
}

GPUModel::HistorySample GPUModel::placeholderSample(double nowSeconds) noexcept
{
    // A default snapshot marked unsampled, so a placeholder records exactly what one did before #1412.
    GPUSnapshot gap;
    gap.captureTimeSec = nowSeconds;
    gap.sampled = false;
    return historySample(gap);
}

GPUModel::GPUSeries::GPUSeries(std::size_t capacity) noexcept
{
    setCapacity(capacity);
}

void GPUModel::GPUSeries::setCapacity(std::size_t capacity) noexcept
{
    timestamps.setCapacity(capacity);
    memoryUsedBytes.setCapacity(capacity);
    memoryTotalBytes.setCapacity(capacity);
    for (auto* series : {&utilization, &memoryPercent, &gpuClock, &encoder, &decoder, &temperature, &power, &fanSpeed})
    {
        series->setCapacity(capacity);
    }
    trailingPlaceholders = std::min(trailingPlaceholders, size());
}

void GPUModel::GPUSeries::reserve(std::size_t count)
{
    // Each reserve either makes room or throws having changed nothing; room made in some series before
    // another throws is unobservable (the same samples, perhaps in a new block).
    timestamps.reserve(count);
    memoryUsedBytes.reserve(count);
    memoryTotalBytes.reserve(count);
    for (auto* series : {&utilization, &memoryPercent, &gpuClock, &encoder, &decoder, &temperature, &power, &fanSpeed})
    {
        series->reserve(count);
    }
}

void GPUModel::GPUSeries::push(const HistorySample& sample)
{
    timestamps.push(sample.timestamp);
    memoryUsedBytes.push(sample.memoryUsedBytes);
    memoryTotalBytes.push(sample.memoryTotalBytes);
    utilization.push(sample.utilization);
    memoryPercent.push(sample.memoryPercent);
    gpuClock.push(sample.gpuClock);
    encoder.push(sample.encoder);
    decoder.push(sample.decoder);
    temperature.push(sample.temperature);
    power.push(sample.power);
    fanSpeed.push(sample.fanSpeed);
    // A full series drops its oldest sample to take this one, so the count can't pass size().
    trailingPlaceholders = sample.sampled ? 0 : std::min(trailingPlaceholders + 1, size());
}

void GPUModel::GPUSeries::discardFront(std::size_t count) noexcept
{
    timestamps.discardFront(count);
    memoryUsedBytes.discardFront(count);
    memoryTotalBytes.discardFront(count);
    for (auto* series : {&utilization, &memoryPercent, &gpuClock, &encoder, &decoder, &temperature, &power, &fanSpeed})
    {
        series->discardFront(count);
    }
    trailingPlaceholders = std::min(trailingPlaceholders, size());
}

GPUPublishedHistory GPUModel::GPUSeries::view() const noexcept
{
    return GPUPublishedHistory{
        .timestamps = timestamps.view(),
        .memoryUsedBytes = memoryUsedBytes.view(),
        .memoryTotalBytes = memoryTotalBytes.view(),
        .utilization = utilization.view(),
        .memoryPercent = memoryPercent.view(),
        .gpuClock = gpuClock.view(),
        .encoder = encoder.view(),
        .decoder = decoder.view(),
        .temperature = temperature.view(),
        .power = power.view(),
        .fanSpeed = fanSpeed.view(),
    };
}

std::vector<GPUSnapshot> orderSnapshotsByEnumeration(std::span<const Platform::GPUInfo> gpuInfo, const GPUSnapshotMap& snapshots)
{
    const auto enumerated = [gpuInfo](std::string_view gpuId)
    {
        return std::ranges::any_of(gpuInfo, [gpuId](const Platform::GPUInfo& info) { return info.id == gpuId; });
    };

    std::vector<GPUSnapshot> ordered;
    ordered.reserve(snapshots.size());
    for (std::size_t index = 0; index < gpuInfo.size(); ++index)
    {
        const std::string& gpuId = gpuInfo[index].id;
        // An id enumerated twice is emitted once, at its first position.
        const auto earlier = gpuInfo.first(index);
        if (std::ranges::any_of(earlier, [&gpuId](const Platform::GPUInfo& info) { return info.id == gpuId; }))
        {
            continue;
        }
        if (const auto it = snapshots.find(gpuId); it != snapshots.end())
        {
            ordered.push_back(it->second);
        }
    }

    // GPUs the read returned but enumeration did not list (a hot-plugged device, or a failed
    // enumeration): after the enumerated ones, by id, so their order is stable too.
    std::vector<const GPUSnapshotMap::value_type*> unlisted;
    for (const auto& entry : snapshots)
    {
        if (!enumerated(entry.first))
        {
            unlisted.push_back(&entry);
        }
    }
    std::ranges::sort(unlisted, {}, [](const GPUSnapshotMap::value_type* entry) { return std::string_view{entry->first}; });
    for (const auto* entry : unlisted)
    {
        ordered.push_back(entry->second);
    }
    return ordered;
}

GPUModel::GPUModel(std::unique_ptr<Platform::IGPUProbe> probe)
    : m_Probe(std::move(probe)), m_PrevSampleTime(std::chrono::steady_clock::now()), m_LastFullRescan(m_PrevSampleTime)
{
    if (!m_Probe)
    {
        // No probe (the synthetic scenario has no GPU, #1413) is a known answer, not an unknown one:
        // no per-process GPU data, so the GPU columns explain that they aren't supported
        // instead of waiting for capabilities that never come.
        spdlog::warn("GPUModel: No GPU probe provided; no per-process GPU data");
        m_CapabilitiesKnown = true;
        m_PerProcessKnownUnsupported.store(true, std::memory_order_release);
        m_PerProcessUtilizationKnownUnsupported.store(true, std::memory_order_release);
        return;
    }

    try
    {
        m_Capabilities = m_Probe->capabilities();
        m_CapabilitiesKnown = true;
        m_PerProcessKnownUnsupported.store(!m_Capabilities.hasPerProcessMetrics, std::memory_order_release);
        m_PerProcessUtilizationKnownUnsupported.store(!m_Capabilities.hasPerProcessUtilization, std::memory_order_release);
    }
    catch (const std::exception& e)
    {
        spdlog::error("GPUModel: Failed to read capabilities: {}", e.what());
    }

    // Enumerate GPUs at construction; refreshAt() re-enumerates when the probe reports a change.
    try
    {
        m_GPUInfo = m_Probe->enumerateGPUs();
        m_GPUInfoKnown = true;
        spdlog::info("GPUModel: Detected {} GPU(s)", m_GPUInfo.size());

        // Initialize history buffers for each GPU
        for (const auto& info : m_GPUInfo)
        {
            m_Histories.try_emplace(info.id, Sampling::historyCapacityForSeconds(m_MaxHistorySeconds));
        }
    }
    catch (const std::exception& e)
    {
        spdlog::error("GPUModel: Failed to enumerate GPUs: {}", e.what());
    }
}

void GPUModel::refresh()
{
    refreshAt(std::chrono::steady_clock::now());
}

void GPUModel::refreshAt(std::chrono::steady_clock::time_point now)
{
    if (!m_Probe)
    {
        return;
    }

    try
    {
        // Read current counters: the system's, then the per-process ones, in one locked section.
        // This sampler is the only one that runs the probe (#1417): the process sampler merges the
        // per-process publication made here, never waiting on the probe or its lock.
        std::vector<Platform::GPUCounters> currentCounters;
        std::exception_ptr systemReadFailure;
        {
            const std::scoped_lock probeLock(m_ProbeMutex);
            // Before the read, so a rebuilt device list and the GPU info describing it arrive together.
            rescanGPUs(now);
            try
            {
                currentCounters = m_Probe->readGPUCounters();
            }
            catch (...)
            {
                // Rethrown below, after the per-process read: one failing read doesn't stop the other.
                systemReadFailure = std::current_exception();
            }
            publishProcessGPUData(now);
        }
        if (systemReadFailure)
        {
            std::rethrow_exception(systemReadFailure);
        }
        const auto currentTime = now;

        // One writer at a time from here to the commit: m_PrevCounters and m_PrevSampleTime are
        // writer-owned, and the publication must be numbered and committed in the order the history
        // was updated (#868). Taken after the probe lock is released: the two are never held together.
        const std::scoped_lock writerLock(m_WriterMutex);

        // Calculate time delta
        auto timeDelta = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - m_PrevSampleTime);
        const double timeDeltaSeconds = static_cast<double>(timeDelta.count()) / 1000.0;

        // Compute snapshots
        SnapshotMap newSnapshots;
        for (const auto& current : currentCounters)
        {
            // Look for previous counters
            const Platform::GPUCounters* previous = nullptr;
            auto prevIt = m_PrevCounters.find(current.gpuId);
            if (prevIt != m_PrevCounters.end())
            {
                previous = &prevIt->second;
            }

            // Compute snapshot
            auto snapshot = computeSnapshot(current, previous, timeDeltaSeconds);
            newSnapshots[current.gpuId] = snapshot;
        }

        // Stamp each sample with this refresh's time, so a GPU's own timestamps stay aligned with its
        // history entries. A local still: nothing observable has changed yet.
        const double nowSec = std::chrono::duration<double>(currentTime.time_since_epoch()).count();
        for (auto& [gpuId, snapshot] : newSnapshots)
        {
            snapshot.captureTimeSec = nowSec;
        }

        // Update stored state under lock, as a transaction (#1412): everything that allocates is
        // staged first, so a std::bad_alloc leaves every series as it was, and still aligned; the
        // commit, the snapshots' move and the trim cannot throw.
        {
            const std::unique_lock lock(m_Mutex);
            PendingHistory pending;
            stageHistoryAppend(pending, newSnapshots);
            commitHistoryAppend(pending, newSnapshots, nowSec);
            m_Snapshots = std::move(newSnapshots);
            trimHistory(nowSec);
        }
        // Outside the exclusive lock: building the publication takes a shared lock only (#868).
        publish();

        // Writer-only state (m_WriterMutex) that no reader touches, so no m_Mutex. Still after
        // publish(), as before #868: a publish that throws leaves the previous baseline in place.
        CounterMap nextPrevious;
        for (const auto& counter : currentCounters)
        {
            auto stored = counter;
            if (const auto before = m_PrevCounters.find(counter.gpuId); before != m_PrevCounters.end())
            {
                carryBusyHighWater(stored, before->second);
            }
            nextPrevious[counter.gpuId] = std::move(stored);
        }
        m_PrevCounters = std::move(nextPrevious);

        m_PrevSampleTime = currentTime;
    }
    catch (const std::exception& e)
    {
        spdlog::error("GPUModel::refresh: {}", e.what());
    }
}

void GPUModel::rescanGPUs(std::chrono::steady_clock::time_point now)
{
    const bool full = (now - m_LastFullRescan) >= std::chrono::seconds(Sampling::GPU_RESCAN_INTERVAL_SECONDS);
    if (full)
    {
        m_LastFullRescan = now;
    }

    bool reenumerate = false;
    try
    {
        reenumerate = m_Probe->rescanGPUs(full ? Platform::GPURescan::Full : Platform::GPURescan::Quick);
    }
    catch (const std::exception& e)
    {
        spdlog::warn("GPUModel: GPU rescan failed: {}", e.what());
    }
    // A startup enumeration or capabilities query that failed is retried, at the full-rescan rate.
    // m_GPUInfoKnown and m_CapabilitiesKnown are only written on this thread, so need no lock to read.
    if (full && (!m_GPUInfoKnown || !m_CapabilitiesKnown))
    {
        reenumerate = true;
    }
    if (!reenumerate)
    {
        return;
    }

    std::optional<Platform::GPUCapabilities> capabilities;
    try
    {
        capabilities = m_Probe->capabilities();
    }
    catch (const std::exception& e)
    {
        spdlog::warn("GPUModel: Failed to re-read capabilities: {}", e.what());
    }
    std::optional<std::vector<Platform::GPUInfo>> gpuInfo;
    try
    {
        gpuInfo = m_Probe->enumerateGPUs();
    }
    catch (const std::exception& e)
    {
        // Keep the GPU info we had: the GPUs it lists that still report counters keep being sampled.
        spdlog::warn("GPUModel: Failed to re-enumerate GPUs: {}", e.what());
    }

    const std::unique_lock lock(m_Mutex);
    if (capabilities.has_value())
    {
        m_Capabilities = *capabilities;
        m_CapabilitiesKnown = true;
        m_PerProcessKnownUnsupported.store(!m_Capabilities.hasPerProcessMetrics, std::memory_order_release);
        m_PerProcessUtilizationKnownUnsupported.store(!m_Capabilities.hasPerProcessUtilization, std::memory_order_release);
    }
    if (gpuInfo.has_value())
    {
        const auto sameIds = std::ranges::equal(m_GPUInfo, *gpuInfo, [](const auto& lhs, const auto& rhs) { return lhs.id == rhs.id; });
        if (!sameIds || !m_GPUInfoKnown)
        {
            spdlog::info("GPUModel: GPU set changed, now {} GPU(s)", gpuInfo->size());
        }
        // A GPU that persists keeps its id, and so its history. A new one gets a history on its first
        // sample; one that is gone stops reporting counters, so its history records gaps until it
        // leaves the window (trimHistory()).
        m_GPUInfo = std::move(*gpuInfo);
        m_GPUInfoKnown = true;
    }
}

void GPUModel::stageHistoryAppend(PendingHistory& pending, const SnapshotMap& snapshots)
{
    // A GPU new to this read gets its series off to the side, with room for this sample. Its history
    // starts here, with no backfill: each GPU's series carry their own timestamps (#1146).
    const std::size_t capacity = Sampling::historyCapacityForSeconds(m_MaxHistorySeconds);
    for (const auto& [gpuId, snapshot] : snapshots)
    {
        if (!m_Histories.contains(gpuId))
        {
            pending.newGpus.try_emplace(gpuId, capacity).first->second.reserve(1);
        }
    }
    // Buckets for the staged GPUs, so moving their nodes in below cannot rehash.
    m_Histories.reserve(m_Histories.size() + pending.newGpus.size());

    // Room for this sample in every existing series, including those of a GPU missing from this read,
    // which gets a placeholder.
    m_HistoryTimestamps.reserve(1);
    for (auto& [gpuId, series] : m_Histories)
    {
        series.reserve(1);
    }
}

// The calls below that could allocate in general (push, node insert) cannot here: each uses room
// stageHistoryAppend() reserved, which the checker can't see.
// NOLINTNEXTLINE(bugprone-exception-escape)
void GPUModel::commitHistoryAppend(PendingHistory& pending, const SnapshotMap& snapshots, double nowSeconds) noexcept
{
    // Adopt the staged series: map nodes spliced in without allocating, into buckets already reserved.
    while (!pending.newGpus.empty())
    {
        m_Histories.insert(pending.newGpus.extract(pending.newGpus.begin()));
    }

    m_HistoryTimestamps.push(nowSeconds);
    // A known GPU missing from this read gets a placeholder, so its history has a gap here rather
    // than a line drawn straight across the absence (#1146).
    const HistorySample placeholder = placeholderSample(nowSeconds);
    for (auto& [gpuId, series] : m_Histories)
    {
        const auto snapshot = snapshots.find(gpuId);
        series.push((snapshot != snapshots.end()) ? historySample(snapshot->second) : placeholder);
    }
}

void GPUModel::setMaxHistorySeconds(double seconds)
{
    const std::scoped_lock writerLock(m_WriterMutex);
    {
        const std::unique_lock lock(m_Mutex);
        m_MaxHistorySeconds = Sampling::clampHistorySeconds(seconds);
        applyHistoryCapacity();
        if (!m_HistoryTimestamps.empty())
        {
            trimHistory(m_HistoryTimestamps.latest());
        }
    }
    // Republish the trimmed history now rather than at the next sample (#1145); see
    // SystemModel::setMaxHistorySeconds(). Nothing is published before the first refresh.
    if (m_PublicationVersion != 0)
    {
        publish();
    }
}

double GPUModel::maxHistorySeconds() const
{
    const std::shared_lock lock(m_Mutex);
    return m_MaxHistorySeconds;
}

void GPUModel::applyHistoryCapacity() noexcept
{
    // Sized for the window at the fastest supported refresh cadence; trimHistory() governs
    // actual retention, as in SystemModel and StorageModel.
    const std::size_t capacity = Sampling::historyCapacityForSeconds(m_MaxHistorySeconds);
    m_HistoryTimestamps.setCapacity(capacity);
    for (auto& [gpuId, series] : m_Histories)
    {
        series.setCapacity(capacity);
    }
}

void GPUModel::trimHistory(double nowSeconds) noexcept
{
    // Like HistoryUtils::discardBefore, keep the newest sample before the cutoff, so the charts'
    // lines run off the window's left edge instead of leaving a strip there (#1016). Only while a
    // newer sample remains: an anchor with nothing after it would be drawn connected to the next
    // sample across the gap. trimCountBefore() applies that rule; discardFront() is O(1).
    const double cutoff = nowSeconds - m_MaxHistorySeconds;
    m_HistoryTimestamps.discardFront(HistoryUtils::trimCountBefore(m_HistoryTimestamps.view(), cutoff));

    // Each GPU has its own timestamps: a refresh it was missing from has a placeholder, but its
    // history starts when it was first seen and is pruned on its own, so it needn't line up with
    // the global timestamps. Trim each GPU by its own timestamps rather than one shared count. A GPU
    // absent for the whole window has no sample after the cutoff, so all of its samples go, rather
    // than keep one that would later be joined to its next sample across the absence.
    for (auto& [gpuId, series] : m_Histories)
    {
        series.discardFront(HistoryUtils::trimCountBefore(series.timestamps.view(), cutoff));
    }

    // A GPU whose window holds nothing but placeholders has been gone for the whole window: forget it,
    // rather than record a placeholder for it on every sample forever.
    std::erase_if(m_Histories, [](const auto& entry) { return !entry.second.hasReading(); });
}

std::shared_ptr<const GPUPublication> GPUModel::publication() const noexcept
{
    return m_Publication.load();
}

std::uint64_t GPUModel::publicationVersion() const noexcept
{
    return m_Publication.version();
}

void GPUModel::publish()
{
    // Build contents first, commit validity keys last: the version comes from a local candidate and
    // m_PublicationVersion only advances once the generation is committed, so a throw from the copies
    // below (std::bad_alloc) leaves the published generation, its version and m_PublicationVersion
    // consistent. The build runs under a shared lock: the per-field accessors still read alongside,
    // and publication() doesn't take m_Mutex at all, so no reader waits for them (#868). The caller
    // holds m_WriterMutex, so no other writer changes the histories meanwhile; rescanGPUs() can still
    // replace the GPU info, but only under the exclusive m_Mutex, so this copy sees all of one list.
    auto publication = std::make_shared<GPUPublication>();
    {
        const std::shared_lock stateLock(m_Mutex);
        publication->version = m_PublicationVersion + 1;
        publication->gpuInfo = m_GPUInfo;
        publication->gpuInfoKnown = m_GPUInfoKnown;
        publication->capabilities = m_Capabilities;
        publication->snapshots = orderSnapshotsByEnumeration(m_GPUInfo, m_Snapshots);
        // A view of each series, sharing the model's samples: O(series), whatever the history length (#1412).
        publication->histories.reserve(m_Histories.size());
        for (const auto& [gpuId, series] : m_Histories)
        {
            publication->histories.emplace(gpuId, series.view());
        }
    }
    const std::uint64_t version = publication->version;
    m_Publication.commit(std::move(publication));
    m_PublicationVersion = version;
}

std::vector<GPUSnapshot> GPUModel::snapshots() const
{
    const std::shared_lock lock(m_Mutex);
    return orderSnapshotsByEnumeration(m_GPUInfo, m_Snapshots);
}

std::vector<Platform::GPUInfo> GPUModel::gpuInfo() const
{
    const std::shared_lock lock(m_Mutex);
    return m_GPUInfo;
}

Platform::GPUCapabilities GPUModel::capabilities() const
{
    const std::shared_lock lock(m_Mutex);
    return m_Capabilities;
}

std::vector<Platform::ProcessGPUCounters> GPUModel::readProcessGPUCounters() const
{
    // m_Capabilities and m_CapabilitiesKnown can be re-read by the sampler thread (rescanGPUs()),
    // so this reads m_PerProcessKnownUnsupported, the atomic flag written alongside them -- not
    // the fields under a shared m_Mutex, which cost this hot early exit ~60% (#1322), nor the
    // probe lock, which a slow probe read holds.
    // The flag folds in m_CapabilitiesKnown deliberately: if the capabilities() query
    // threw, m_Capabilities is left at its default (all-false) values, and treating that as
    // "confirmed unsupported" would permanently and silently suppress a probe that might
    // genuinely support per-process data, just because of a one-time query failure. Only
    // skip the probe lock when discovery actually succeeded and reported no support --
    // e.g. Linux Intel DRM, which always returns empty here. When discovery failed, fall
    // through to the lock-and-call path unconditionally, matching this method's behavior
    // before this capability check existed.
    ProcessGPUReading reading = readProcessGPUData();
    if (reading.failure)
    {
        std::rethrow_exception(reading.failure);
    }
    return std::move(reading.counters);
}

GPUModel::ProcessGPUReading GPUModel::readProcessGPUData() const
{
    if (!m_Probe)
    {
        return {};
    }
    // The hot early exit described above: unsupported, and nothing read, from the same load.
    if (m_PerProcessKnownUnsupported.load(std::memory_order_acquire))
    {
        return {};
    }
    const std::scoped_lock probeLock(m_ProbeMutex);
    return readProcessGPUDataLocked();
}

GPUModel::ProcessGPUReading GPUModel::readProcessGPUDataLocked() const
{
    // Read under the probe lock: rescanGPUs(), which can change the flags, runs holding it, so
    // these are the flags the read below happens under (#1210).
    ProcessGPUReading reading;
    reading.perProcessSupported = !m_PerProcessKnownUnsupported.load(std::memory_order_acquire);
    if (!reading.perProcessSupported)
    {
        return reading;
    }
    reading.utilizationSupported = !m_PerProcessUtilizationKnownUnsupported.load(std::memory_order_acquire);
    try
    {
        reading.counters = m_Probe->readProcessGPUCounters();
    }
    catch (...)
    {
        // Carried to the caller with the flags above, which this failed read ran under (#1210).
        reading.failure = std::current_exception();
    }
    return reading;
}

void GPUModel::publishProcessGPUData(std::chrono::steady_clock::time_point now)
{
    ProcessGPUReading reading = readProcessGPUDataLocked();
    if (reading.failure && !m_ProcessReadFailing)
    {
        // Once per streak; the publication below carries the failure to ProcessModel (#1142, #1210).
        try
        {
            std::rethrow_exception(reading.failure);
        }
        catch (const std::exception& e)
        {
            spdlog::warn("GPUModel: reading per-process GPU data failed; publishing a gap: {}", e.what());
        }
        catch (...)
        {
            spdlog::warn("GPUModel: reading per-process GPU data failed; publishing a gap");
        }
    }
    else if (!reading.failure && m_ProcessReadFailing)
    {
        spdlog::info("GPUModel: per-process GPU data is being read again");
    }
    m_ProcessReadFailing = static_cast<bool>(reading.failure);

    try
    {
        auto publication = std::make_shared<ProcessGPUPublication>();
        publication->version = m_ProcessPublicationVersion + 1;
        publication->captureTime = now;
        publication->perProcessSupported = reading.perProcessSupported;
        publication->utilizationSupported = reading.utilizationSupported;
        publication->readFailed = reading.perProcessSupported && static_cast<bool>(reading.failure);
        publication->counters = std::move(reading.counters);
        // After construction m_GPUInfo is written only by rescanGPUs(), under m_ProbeMutex, which is
        // held here: it needs no m_Mutex.
        publication->adapters.reserve(m_GPUInfo.size());
        for (const auto& info : m_GPUInfo)
        {
            publication->adapters.push_back(GPUAdapterIdentity{.id = info.id,
                                                               .luidId = info.luidId,
                                                               .name = info.name,
                                                               .isIntegrated = info.isIntegrated,
                                                               .memoryIsShared = info.memoryIsShared});
        }
        const std::uint64_t version = publication->version;
        m_ProcessPublication.commit(std::move(publication));
        m_ProcessPublicationVersion = version;
    }
    catch (const std::exception& e)
    {
        // The previous publication stays; it ages until ProcessModel treats it as stale.
        spdlog::error("GPUModel: publishing per-process GPU data failed: {}", e.what());
    }
}

GPUSnapshot
GPUModel::computeSnapshot(const Platform::GPUCounters& current, const Platform::GPUCounters* previous, double timeDeltaSeconds) const
{
    GPUSnapshot snapshot;

    // Copy identity
    snapshot.gpuId = current.gpuId;

    // Find GPU info for this ID
    auto infoIt = std::ranges::find_if(m_GPUInfo, [&](const auto& info) { return info.id == current.gpuId; });
    if (infoIt != m_GPUInfo.end())
    {
        snapshot.name = infoIt->name;
        snapshot.vendor = infoIt->vendor;
        snapshot.isIntegrated = infoIt->isIntegrated;
        snapshot.memoryIsShared = infoIt->memoryIsShared;
        snapshot.luidId = infoIt->luidId; // For PDH counter matching
    }

    // Copy instantaneous values
    snapshot.utilizationAvailable = current.utilizationAvailable;
    snapshot.temperatureAvailable = current.temperatureAvailable;
    snapshot.powerAvailable = current.powerAvailable;
    snapshot.gpuClockAvailable = current.gpuClockAvailable;
    snapshot.memoryAvailable = current.memoryAvailable;
    snapshot.suspended = current.suspended;
    snapshot.utilizationPercent = current.utilizationPercent;
    snapshot.memoryUsedBytes = current.memoryUsedBytes;
    snapshot.memoryTotalBytes = current.memoryTotalBytes;
    snapshot.temperatureC = current.temperatureC;
    snapshot.powerDrawWatts = current.powerDrawWatts;
    snapshot.powerLimitWatts = current.powerLimitWatts;
    snapshot.gpuClockMHz = current.gpuClockMHz;
    snapshot.encoderUtilPercent = current.encoderUtilPercent;
    snapshot.decoderUtilPercent = current.decoderUtilPercent;

    // Power from a cumulative energy counter (#1269): its change over the sample interval. Without
    // a readable previous counter, or when it went backwards (a driver reload), power is unread.
    if (current.energyAvailable)
    {
        const bool haveDelta = previous != nullptr && previous->energyAvailable && timeDeltaSeconds > 0.0 &&
                               current.energyMicroJoules >= previous->energyMicroJoules;
        constexpr double MICROJOULES_PER_JOULE = 1'000'000.0;
        snapshot.powerAvailable = haveDelta;
        snapshot.powerDrawWatts =
            haveDelta
                ? Numeric::counterRate(current.energyMicroJoules, previous->energyMicroJoules, timeDeltaSeconds) / MICROJOULES_PER_JOULE
                : 0.0;
    }

    // Utilization from the DRM clients' engine busyness (#1267): their change since the previous
    // sample. Without a previous reading (the first sample, or after a suspend or a failed read), unread.
    if (current.engineBusyAvailable)
    {
        const bool haveDelta = previous != nullptr && previous->engineBusyAvailable;
        snapshot.utilizationAvailable = haveDelta;
        snapshot.utilizationPercent = haveDelta ? engineUtilizationPercent(current.engineClients, previous->engineClients) : 0.0;
    }

    // Compute derived values
    if (current.memoryTotalBytes > 0)
    {
        snapshot.memoryUsedPercent = (static_cast<double>(current.memoryUsedBytes) / static_cast<double>(current.memoryTotalBytes)) * 100.0;
    }

    // Fan speed: normalize the vendor-native raw reading against the device's own max here in
    // Domain, not in the Platform probe, matching memoryUsedPercent above (see
    // #734 review discussion -- GPUCounters holds unconverted raw values only). Left unclamped
    // to 100, like memoryUsedPercent above: a raw reading above the device's
    // reported max is itself useful signal (sensor drift, transient overspeed), not something to
    // silently cap.
    snapshot.fanSpeedAvailable = current.fanSpeedMaxRaw > 0;
    if (snapshot.fanSpeedAvailable)
    {
        const std::uint64_t percent = (static_cast<std::uint64_t>(current.fanSpeedRaw) * 100ULL) / current.fanSpeedMaxRaw;
        // Cap at uint32_t's range rather than at 100 via the existing narrow-or-fallback helper:
        // a wildly out-of-range percent (e.g. a corrupted fanSpeedMaxRaw of 1) would otherwise
        // silently truncate/wrap to an arbitrary small value on a plain narrowing cast, hiding
        // exactly the sensor-drift/overspeed signal this computation is meant to preserve.
        snapshot.fanSpeedPercent = Numeric::narrowOr<std::uint32_t>(percent, std::numeric_limits<std::uint32_t>::max());
    }

    return snapshot;
}

template<typename T> std::vector<T> GPUModel::copySeries(std::string_view gpuId, SharedHistoryBuffer<T> GPUSeries::* series) const
{
    const std::shared_lock lock(m_Mutex);
    const auto it = m_Histories.find(gpuId);
    if (it == m_Histories.end())
    {
        return {};
    }
    return HistoryUtils::toVector(it->second.*series);
}

// The same values the publication carries: NaN where a reading failed or the GPU was missing.
std::vector<float> GPUModel::utilizationHistory(std::string_view gpuId) const
{
    return copySeries(gpuId, &GPUSeries::utilization);
}

std::vector<float> GPUModel::memoryPercentHistory(std::string_view gpuId) const
{
    return copySeries(gpuId, &GPUSeries::memoryPercent);
}

std::vector<float> GPUModel::gpuClockHistory(std::string_view gpuId) const
{
    return copySeries(gpuId, &GPUSeries::gpuClock);
}

std::vector<float> GPUModel::encoderHistory(std::string_view gpuId) const
{
    return copySeries(gpuId, &GPUSeries::encoder);
}

std::vector<float> GPUModel::decoderHistory(std::string_view gpuId) const
{
    return copySeries(gpuId, &GPUSeries::decoder);
}

std::vector<float> GPUModel::temperatureHistory(std::string_view gpuId) const
{
    return copySeries(gpuId, &GPUSeries::temperature);
}

std::vector<float> GPUModel::powerHistory(std::string_view gpuId) const
{
    return copySeries(gpuId, &GPUSeries::power);
}

std::vector<float> GPUModel::fanSpeedHistory(std::string_view gpuId) const
{
    // NaN, not 0.0F, where the fan couldn't be read, so a caller doesn't see a misleading flat "0%".
    return copySeries(gpuId, &GPUSeries::fanSpeed);
}

std::vector<double> GPUModel::historyTimestamps() const
{
    const std::shared_lock lock(m_Mutex);
    return HistoryUtils::toVector(m_HistoryTimestamps);
}

std::vector<double> GPUModel::historyTimestamps(std::string_view gpuId) const
{
    return copySeries(gpuId, &GPUSeries::timestamps);
}

} // namespace Domain
