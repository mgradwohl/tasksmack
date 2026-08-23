#include "StorageModel.h"

#include "Domain/StorageSnapshot.h"
#include "History.h"
#include "Numeric.h"
#include "Platform/IDiskProbe.h"
#include "Platform/StorageTypes.h"
#include "SamplingConfig.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Domain
{

StorageModel::StorageModel(std::unique_ptr<Platform::IDiskProbe> probe)
    : m_Probe(std::move(probe)), m_StartTime(std::chrono::steady_clock::now())
{}

void StorageModel::sample()
{
    if (!m_Probe)
    {
        spdlog::warn("StorageModel::sample called with null probe");
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    // Use absolute time (since epoch) to match SystemModel's timestamp format
    const double nowSeconds = std::chrono::duration<double>(now.time_since_epoch()).count();

    const Platform::SystemDiskCounters counters = m_Probe->read();
    const Platform::DiskCapabilities caps = m_Probe->capabilities();

    StorageSnapshot snapshot;
    snapshot.hasDiskStats = caps.hasDiskStats;
    snapshot.hasReadWriteBytes = caps.hasReadWriteBytes;
    snapshot.hasIoTime = caps.hasIoTime;

    // Process each disk
    snapshot.disks.reserve(counters.disks.size());
    for (const auto& diskCounters : counters.disks)
    {
        const std::string& deviceName = diskCounters.deviceName;

        // Get or create state for this device
        auto& state = m_DiskStates[deviceName];
        state.deviceName = deviceName;

        const DiskSnapshot diskSnap = computeDiskSnapshot(diskCounters, state);
        snapshot.disks.push_back(diskSnap);

        // Update state for next sample
        state.prevCounters = diskCounters;
        state.prevTime = now;

        // If this is the very first time we've seen this disk (the seed read),
        // mark the next sample as a seed transition to prevent rate spikes.
        if (!state.hasPrev)
        {
            state.isSeedTransition = true;
        }
        else
        {
            state.isSeedTransition = false;
        }

        state.hasPrev = true;
    }

    // Compute system-wide totals
    for (const auto& disk : snapshot.disks)
    {
        snapshot.totalReadBytesPerSec += disk.readBytesPerSec;
        snapshot.totalWriteBytesPerSec += disk.writeBytesPerSec;
        snapshot.totalReadOpsPerSec += disk.readOpsPerSec;
        snapshot.totalWriteOpsPerSec += disk.writeOpsPerSec;
    }

    // Update shared state
    {
        std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
        m_LatestSnapshot = snapshot;
        m_History.push_back(snapshot);
        m_Timestamps.push_back(nowSeconds);

        // Maintain per-disk I/O histories aligned to m_Timestamps.
        // Track which disks are present in this sample; known-but-absent disks
        // get a zero placeholder so every deque stays index-aligned with m_Timestamps.
        std::unordered_set<std::string> presentDisks;
        presentDisks.reserve(snapshot.disks.size());
        for (const auto& disk : snapshot.disks)
        {
            const auto& name = disk.deviceName;
            presentDisks.insert(name);
            if (!m_DiskReadHistory.contains(name))
            {
                // New disk: backfill zeros for all prior timestamps before this sample.
                m_DiskOrder.push_back(name);
                const size_t backfillCount = m_Timestamps.size() - 1; // current ts already pushed
                for (size_t i = 0; i < backfillCount; ++i)
                {
                    m_DiskReadHistory[name].push_back(0.0);
                    m_DiskWriteHistory[name].push_back(0.0);
                }
            }
            m_DiskReadHistory[name].push_back(disk.readBytesPerSec);
            m_DiskWriteHistory[name].push_back(disk.writeBytesPerSec);
        }
        // Append a zero placeholder for known disks absent from this sample.
        for (const auto& name : m_DiskOrder)
        {
            if (!presentDisks.contains(name))
            {
                m_DiskReadHistory[name].push_back(0.0);
                m_DiskWriteHistory[name].push_back(0.0);
            }
        }

        trimHistory(nowSeconds);
        publish();
        m_HasPrevSample = true;
        m_PrevSampleTime = now;
    }

    spdlog::trace("StorageModel: sampled {} disks, total read: {:.2f} MB/s, write: {:.2f} MB/s",
                  snapshot.disks.size(),
                  snapshot.totalReadBytesPerSec / (1024.0 * 1024.0),
                  snapshot.totalWriteBytesPerSec / (1024.0 * 1024.0));
}

std::shared_ptr<const StoragePublication> StorageModel::publication() const noexcept
{
    return m_Publication.load(std::memory_order_acquire);
}

void StorageModel::publish()
{
    auto publication = std::make_shared<StoragePublication>();
    publication->version = ++m_PublicationVersion;
    publication->snapshot = m_LatestSnapshot;
    publication->timestamps = HistoryUtils::toVector(m_Timestamps);
    publication->totalReadHistory.reserve(m_History.size());
    publication->totalWriteHistory.reserve(m_History.size());
    for (const auto& snapshot : m_History)
    {
        publication->totalReadHistory.push_back(snapshot.totalReadBytesPerSec);
        publication->totalWriteHistory.push_back(snapshot.totalWriteBytesPerSec);
    }
    publication->perDiskHistory.reserve(m_DiskOrder.size());
    for (const auto& name : m_DiskOrder)
    {
        publication->perDiskHistory.push_back({
            .deviceName = name,
            .readBytesPerSec = HistoryUtils::toVector(m_DiskReadHistory.at(name)),
            .writeBytesPerSec = HistoryUtils::toVector(m_DiskWriteHistory.at(name)),
        });
    }
    m_Publication.store(std::move(publication), std::memory_order_release);
}

DiskSnapshot StorageModel::computeDiskSnapshot(const Platform::DiskCounters& current, DiskState& state)
{
    DiskSnapshot snap;
    snap.deviceName = current.deviceName;
    snap.isPhysicalDevice = current.isPhysicalDevice;

    // Set cumulative totals
    snap.totalReadBytes = current.readSectors * current.sectorSize;
    snap.totalWriteBytes = current.writeSectors * current.sectorSize;
    snap.totalReadOps = current.readsCompleted;
    snap.totalWriteOps = current.writesCompleted;

    if (!state.hasPrev)
    {
        // First sample, can't compute rates yet.
        return snap;
    }

    // Compute deltas
    const auto deltaTime = std::chrono::steady_clock::now() - state.prevTime;
    const double deltaSeconds = std::chrono::duration<double>(deltaTime).count();

    // The sampler's first callback can immediately follow the synchronous seed read.
    // Suppress only that implausibly short seed transition; normal second samples
    // should produce rates without requiring a third observation.
    constexpr double MIN_SEED_ELAPSED_SECONDS = static_cast<double>(Sampling::REFRESH_INTERVAL_MIN_MS) / 2000.0;
    if (deltaSeconds <= 0.0 || (state.isSeedTransition && deltaSeconds < MIN_SEED_ELAPSED_SECONDS))
    {
        return snap;
    }

    const std::uint64_t deltaReadSectors = Numeric::counterDelta(current.readSectors, state.prevCounters.readSectors);
    const std::uint64_t deltaWriteSectors = Numeric::counterDelta(current.writeSectors, state.prevCounters.writeSectors);
    const std::uint64_t deltaReadOps = Numeric::counterDelta(current.readsCompleted, state.prevCounters.readsCompleted);
    const std::uint64_t deltaWriteOps = Numeric::counterDelta(current.writesCompleted, state.prevCounters.writesCompleted);
    const std::uint64_t deltaReadTime = Numeric::counterDelta(current.readTimeMs, state.prevCounters.readTimeMs);
    const std::uint64_t deltaWriteTime = Numeric::counterDelta(current.writeTimeMs, state.prevCounters.writeTimeMs);
    const std::uint64_t deltaIoTime = Numeric::counterDelta(current.ioTimeMs, state.prevCounters.ioTimeMs);

    // Compute rates
    snap.readBytesPerSec = static_cast<double>(deltaReadSectors * current.sectorSize) / deltaSeconds;
    snap.writeBytesPerSec = static_cast<double>(deltaWriteSectors * current.sectorSize) / deltaSeconds;
    snap.readOpsPerSec = Numeric::toDouble(deltaReadOps) / deltaSeconds;
    snap.writeOpsPerSec = Numeric::toDouble(deltaWriteOps) / deltaSeconds;

    // Compute average I/O times
    if (deltaReadOps > 0)
    {
        snap.avgReadTimeMs = static_cast<double>(deltaReadTime) / static_cast<double>(deltaReadOps);
    }
    if (deltaWriteOps > 0)
    {
        snap.avgWriteTimeMs = static_cast<double>(deltaWriteTime) / static_cast<double>(deltaWriteOps);
    }

    // Compute utilization (percentage of time the device was busy)
    snap.utilizationPercent = (static_cast<double>(deltaIoTime) / (deltaSeconds * 1000.0)) * 100.0;
    snap.utilizationPercent = std::clamp(snap.utilizationPercent, 0.0, 100.0);

    return snap;
}

void StorageModel::trimHistory(double nowSeconds)
{
    const double cutoff = nowSeconds - m_MaxHistorySeconds;
    static_cast<void>(HistoryUtils::trimBefore(m_Timestamps, cutoff, m_History));

    // Align per-disk deques to the timestamp count so each disk series remains
    // index-aligned with m_Timestamps even when a disk is introduced later or
    // missing from some samples.
    // Use .find() rather than operator[] to avoid unintentionally inserting
    // empty deques for names that do not yet have map entries.
    const size_t targetSize = m_Timestamps.size();
    for (const auto& name : m_DiskOrder)
    {
        if (auto readIt = m_DiskReadHistory.find(name); readIt != m_DiskReadHistory.end())
        {
            auto& readHist = readIt->second;
            HistoryUtils::alignFrontToSize(readHist, targetSize);
        }
        if (auto writeIt = m_DiskWriteHistory.find(name); writeIt != m_DiskWriteHistory.end())
        {
            auto& writeHist = writeIt->second;
            HistoryUtils::alignFrontToSize(writeHist, targetSize);
        }
    }
}

StorageSnapshot StorageModel::latestSnapshot() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return m_LatestSnapshot;
}

std::vector<StorageSnapshot> StorageModel::history() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_History);
}

std::vector<double> StorageModel::totalReadHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    std::vector<double> out;
    out.reserve(m_History.size());
    for (const auto& snap : m_History)
    {
        out.push_back(snap.totalReadBytesPerSec);
    }
    return out;
}

std::vector<double> StorageModel::totalWriteHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    std::vector<double> out;
    out.reserve(m_History.size());
    for (const auto& snap : m_History)
    {
        out.push_back(snap.totalWriteBytesPerSec);
    }
    return out;
}

std::vector<PerDiskHistory> StorageModel::perDiskHistory() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    std::vector<PerDiskHistory> result;
    result.reserve(m_DiskOrder.size());
    for (const auto& name : m_DiskOrder)
    {
        PerDiskHistory entry;
        entry.deviceName = name;
        const auto readIt = m_DiskReadHistory.find(name);
        const auto writeIt = m_DiskWriteHistory.find(name);
        if (readIt != m_DiskReadHistory.end())
        {
            entry.readBytesPerSec = {readIt->second.begin(), readIt->second.end()};
        }
        if (writeIt != m_DiskWriteHistory.end())
        {
            entry.writeBytesPerSec = {writeIt->second.begin(), writeIt->second.end()};
        }
        result.push_back(std::move(entry));
    }
    return result;
}

std::vector<double> StorageModel::historyTimestamps() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return HistoryUtils::toVector(m_Timestamps);
}

void StorageModel::setMaxHistorySeconds(double seconds)
{
    std::unique_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    m_MaxHistorySeconds = seconds;
}

Platform::DiskCapabilities StorageModel::capabilities() const
{
    if (m_Probe)
    {
        return m_Probe->capabilities();
    }
    return Platform::DiskCapabilities{};
}

} // namespace Domain
