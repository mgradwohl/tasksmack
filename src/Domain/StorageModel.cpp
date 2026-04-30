#include "StorageModel.h"

#include "Domain/StorageSnapshot.h"
#include "Platform/IDiskProbe.h"
#include "Platform/StorageTypes.h"

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
        m_HasPrevSample = true;
        m_PrevSampleTime = now;
    }

    spdlog::trace("StorageModel: sampled {} disks, total read: {:.2f} MB/s, write: {:.2f} MB/s",
                  snapshot.disks.size(),
                  snapshot.totalReadBytesPerSec / (1024.0 * 1024.0),
                  snapshot.totalWriteBytesPerSec / (1024.0 * 1024.0));
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
        // First sample, can't compute rates yet
        return snap;
    }

    // Compute deltas
    const auto deltaTime = std::chrono::steady_clock::now() - state.prevTime;
    const double deltaSeconds = std::chrono::duration<double>(deltaTime).count();

    if (deltaSeconds <= 0.0)
    {
        return snap;
    }

    const uint64_t deltaReadSectors =
        (current.readSectors >= state.prevCounters.readSectors) ? (current.readSectors - state.prevCounters.readSectors) : 0;

    const uint64_t deltaWriteSectors =
        (current.writeSectors >= state.prevCounters.writeSectors) ? (current.writeSectors - state.prevCounters.writeSectors) : 0;

    const uint64_t deltaReadOps =
        (current.readsCompleted >= state.prevCounters.readsCompleted) ? (current.readsCompleted - state.prevCounters.readsCompleted) : 0;

    const uint64_t deltaWriteOps = (current.writesCompleted >= state.prevCounters.writesCompleted)
                                     ? (current.writesCompleted - state.prevCounters.writesCompleted)
                                     : 0;

    const uint64_t deltaReadTime =
        (current.readTimeMs >= state.prevCounters.readTimeMs) ? (current.readTimeMs - state.prevCounters.readTimeMs) : 0;

    const uint64_t deltaWriteTime =
        (current.writeTimeMs >= state.prevCounters.writeTimeMs) ? (current.writeTimeMs - state.prevCounters.writeTimeMs) : 0;

    const uint64_t deltaIoTime = (current.ioTimeMs >= state.prevCounters.ioTimeMs) ? (current.ioTimeMs - state.prevCounters.ioTimeMs) : 0;

    // Compute rates
    snap.readBytesPerSec = static_cast<double>(deltaReadSectors * current.sectorSize) / deltaSeconds;
    snap.writeBytesPerSec = static_cast<double>(deltaWriteSectors * current.sectorSize) / deltaSeconds;
    snap.readOpsPerSec = static_cast<double>(deltaReadOps) / deltaSeconds;
    snap.writeOpsPerSec = static_cast<double>(deltaWriteOps) / deltaSeconds;

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
    while (!m_Timestamps.empty() && (m_Timestamps.front() < cutoff))
    {
        m_Timestamps.pop_front();
        m_History.pop_front();
    }

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
            while (readHist.size() > targetSize)
            {
                readHist.pop_front();
            }
            while (readHist.size() < targetSize)
            {
                readHist.push_front({});
            }
        }
        if (auto writeIt = m_DiskWriteHistory.find(name); writeIt != m_DiskWriteHistory.end())
        {
            auto& writeHist = writeIt->second;
            while (writeHist.size() > targetSize)
            {
                writeHist.pop_front();
            }
            while (writeHist.size() < targetSize)
            {
                writeHist.push_front({});
            }
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
    return {m_History.begin(), m_History.end()};
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
    return {m_Timestamps.begin(), m_Timestamps.end()};
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
