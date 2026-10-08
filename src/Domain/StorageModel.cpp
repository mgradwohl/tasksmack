#include "StorageModel.h"

#include "Domain/StorageSnapshot.h"
#include "History.h"
#include "Numeric.h"
#include "Platform/IDiskProbe.h"
#include "Platform/StorageTypes.h"
#include "PublicationSlot.h"
#include "SamplingConfig.h"
#include "SharedHistory.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Domain
{

namespace
{
/// A system-wide rate for the history: NaN when no disk had measured rates in that sample (the
/// seed transition), so the chart shows a gap rather than a false 0 B/s (#1102), and when any disk's
/// sample was thrown out as a counter glitch (#1291), whose missing share would otherwise plot a
/// false dip in the Total.
[[nodiscard]] double totalRateOrNaN(const StorageSnapshot& snapshot, double StorageSnapshot::* total) noexcept
{
    return snapshot.totalsMeasured ? snapshot.*total : std::numeric_limits<double>::quiet_NaN();
}
} // namespace

StorageModel::StorageModel(std::unique_ptr<Platform::IDiskProbe> probe) : m_Probe(std::move(probe))
{
    applyHistoryCapacity();
}

void StorageModel::applyHistoryCapacity()
{
    // Size every ring so the configured time window fits even at the fastest
    // supported refresh cadence; time-based trimming governs actual retention. The shared series
    // resize at their next compaction.
    const std::size_t capacity = Sampling::historyCapacityForSeconds(m_MaxHistorySeconds);
    m_Timestamps.setCapacity(capacity);
    m_TotalReadHistory.setCapacity(capacity);
    m_TotalWriteHistory.setCapacity(capacity);
    for (auto& [name, history] : m_DiskReadHistory)
    {
        history.setCapacity(capacity);
    }
    for (auto& [name, history] : m_DiskWriteHistory)
    {
        history.setCapacity(capacity);
    }
}

void StorageModel::sample()
{
    sampleAt(std::chrono::steady_clock::now());
}

void StorageModel::sampleAt(const std::chrono::steady_clock::time_point now)
{
    if (!m_Probe)
    {
        spdlog::warn("StorageModel::sampleAt called with null probe");
        return;
    }

    // Read first, then ask for capabilities: a read can re-enumerate the disks and change what the
    // probe reports (Windows), and one expression would leave the order unspecified.
    const Platform::SystemDiskCounters counters = m_Probe->read();
    const Platform::DiskCapabilities caps = m_Probe->capabilities();

    // One writer at a time from here to the commit: m_DiskStates is writer-owned, and the
    // publication must be numbered and committed in the order the history was updated.
    const std::scoped_lock writerLock(m_WriterMutex);
    applySample(counters, caps, now, /*publishNow=*/true);
}

void StorageModel::sampleSeries(const CounterSeriesSource& next)
{
    const Platform::DiskCapabilities caps = m_Probe ? m_Probe->capabilities() : Platform::DiskCapabilities{};
    Platform::SystemDiskCounters counters;
    std::chrono::steady_clock::time_point now{};
    bool applied = false;
    // The whole series is one write: no other writer interleaves, and its single publish below is
    // numbered and committed after every sample it holds (#868).
    const std::scoped_lock writerLock(m_WriterMutex);
    while (next(counters, now))
    {
        {
            const std::shared_lock lock(m_Mutex);
            if (!m_Timestamps.empty() && std::chrono::duration<double>(now.time_since_epoch()).count() <= m_Timestamps.latest())
            {
                continue;
            }
        }
        applySample(counters, caps, now, /*publishNow=*/false);
        applied = true;
    }
    if (applied)
    {
        publish(); // builds under a shared lock, then commits through the publication slot
    }
}

void StorageModel::applySample(const Platform::SystemDiskCounters& counters,
                               const Platform::DiskCapabilities& caps,
                               const std::chrono::steady_clock::time_point now,
                               const bool publishNow)
{
    // Use absolute time (since epoch) to match SystemModel's timestamp format
    const double nowSeconds = std::chrono::duration<double>(now.time_since_epoch()).count();

    // The sample's state and history update is a transaction (#1412): everything it allocates -- the
    // next per-disk rate state, the snapshot copies, new disks' series and every reservation -- is made
    // first, off to the side, so a std::bad_alloc before the commit leaves the history, rate state and
    // snapshot as they were; then it is committed without throwing, so every series advances together
    // or none does. publish() below still allocates: a throw there leaves the history one sample ahead
    // of an unchanged publication, which the next successful publish catches up.
    PendingSample pending;
    // m_DiskStates is writer-owned (readers never touch it), so it is copied and updated without a
    // lock and swapped in at the commit.
    pending.diskStates = m_DiskStates;
    pending.latest = computeSnapshot(counters, caps, now, pending.diskStates);

    {
        const std::unique_lock lock(m_Mutex);
        stageHistoryAppend(pending, pending.latest, nowSeconds);
        commitHistoryAppend(pending, nowSeconds);
    }
    if (publishNow)
    {
        // Outside the exclusive lock: publish() reads under a shared lock only (#868).
        publish();
    }

    spdlog::trace("StorageModel: sampled {} disks, total read: {:.2f} MB/s, write: {:.2f} MB/s",
                  m_LatestSnapshot.disks.size(),
                  m_LatestSnapshot.totalReadBytesPerSec / (1024.0 * 1024.0),
                  m_LatestSnapshot.totalWriteBytesPerSec / (1024.0 * 1024.0));
}

StorageSnapshot StorageModel::computeSnapshot(const Platform::SystemDiskCounters& counters,
                                              const Platform::DiskCapabilities& caps,
                                              const std::chrono::steady_clock::time_point now,
                                              std::unordered_map<std::string, DiskState>& diskStates)
{
    StorageSnapshot snapshot;
    snapshot.hasDiskStats = caps.hasDiskStats;
    snapshot.hasReadWriteBytes = caps.hasReadWriteBytes;
    snapshot.hasIoTime = caps.hasIoTime;

    // Process each disk. A device name is one disk: only its first entry in the sample counts. It
    // advances the rate state (so the next sample is measured from it), is listed in the snapshot, is
    // what the history records and is all the totals add. A later entry with the same name (a
    // duplicated /proc/diskstats row, a drive the Windows probe lists twice) is dropped, so the
    // snapshot, the per-disk history and the totals all count the disk once (#1467).
    snapshot.disks.reserve(counters.disks.size());
    std::unordered_set<std::string> seen;
    seen.reserve(counters.disks.size());
    for (const auto& diskCounters : counters.disks)
    {
        const std::string& deviceName = diskCounters.deviceName;
        if (!seen.insert(deviceName).second)
        {
            continue;
        }

        // Get or create state for this device
        auto& state = diskStates[deviceName];
        state.deviceName = deviceName;

        const DiskSnapshot diskSnap = computeDiskSnapshot(diskCounters, state, now);
        snapshot.disks.push_back(diskSnap);

        // Update state for next sample
        state.prevCounters = diskCounters;
        state.prevTime = now;

        // If this is the very first time we've seen this disk (the seed read),
        // mark the next sample as a seed transition to prevent rate spikes.
        state.isSeedTransition = !state.hasPrev;

        state.hasPrev = true;
    }

    // Compute system-wide totals, and whether they are a measurement (see totalRateOrNaN()). Each
    // device name is in snapshot.disks once, so no disk is added twice.
    bool anyRates = false;
    bool anyRejected = false;
    for (const auto& disk : snapshot.disks)
    {
        snapshot.totalReadBytesPerSec += disk.readBytesPerSec;
        snapshot.totalWriteBytesPerSec += disk.writeBytesPerSec;
        snapshot.totalReadOpsPerSec += disk.readOpsPerSec;
        snapshot.totalWriteOpsPerSec += disk.writeOpsPerSec;
        anyRates = anyRates || disk.hasRates;
        anyRejected = anyRejected || disk.ratesRejected;
    }
    snapshot.totalsMeasured = anyRates && !anyRejected;
    return snapshot;
}

void StorageModel::stageHistoryAppend(PendingSample& pending, const StorageSnapshot& snapshot, double nowSeconds)
{
    // A new disk's series is backfilled with NaN -- no reading, drawn as a gap (#1015) -- to the length
    // of m_Timestamps (clamped to its capacity), so it is aligned before this sample is added.
    const std::size_t capacity = Sampling::historyCapacityForSeconds(m_MaxHistorySeconds);
    const std::size_t backfillCount = std::min(m_Timestamps.size(), capacity - 1);
    const auto makeBackfilled = [capacity, backfillCount]
    {
        SharedHistoryBuffer<double> history(capacity);
        history.reserve(backfillCount + 1); // room for this sample too
        for (std::size_t i = 0; i < backfillCount; ++i)
        {
            history.push(std::numeric_limits<double>::quiet_NaN());
        }
        return history;
    };

    // Which names this sample has (computeSnapshot() lists each device name once, so each series is
    // appended to once and stays aligned). New names get their series, display slot and last-seen
    // entry staged here.
    pending.present.reserve(snapshot.disks.size());
    for (const auto& disk : snapshot.disks)
    {
        pending.present.insert(disk.deviceName);
        if (!m_DiskReadHistory.contains(disk.deviceName))
        {
            pending.newRead.emplace(disk.deviceName, makeBackfilled());
            pending.newWrite.emplace(disk.deviceName, makeBackfilled());
            pending.newOrder.push_back(disk.deviceName);
        }
        if (!m_DiskLastSeenSeconds.contains(disk.deviceName))
        {
            pending.newLastSeen.emplace(disk.deviceName, nowSeconds);
        }
    }
    // Buckets and slots for the staged entries, so moving them in at the commit cannot allocate.
    m_DiskReadHistory.reserve(m_DiskReadHistory.size() + pending.newRead.size());
    m_DiskWriteHistory.reserve(m_DiskWriteHistory.size() + pending.newWrite.size());
    m_DiskLastSeenSeconds.reserve(m_DiskLastSeenSeconds.size() + pending.newLastSeen.size());
    m_DiskOrder.reserve(m_DiskOrder.size() + pending.newOrder.size());

    // Room for this sample in every existing series.
    for (auto* history : {&m_Timestamps, &m_TotalReadHistory, &m_TotalWriteHistory})
    {
        history->reserve(1);
    }
    for (auto& [name, history] : m_DiskReadHistory)
    {
        history.reserve(1);
    }
    for (auto& [name, history] : m_DiskWriteHistory)
    {
        history.reserve(1);
    }
}

// The calls below that could allocate in general (push, push_back, node insert) cannot here: each
// uses room stageHistoryAppend() reserved, which the checker can't see.
// NOLINTNEXTLINE(bugprone-exception-escape)
void StorageModel::commitHistoryAppend(PendingSample& pending, double nowSeconds) noexcept
{
    const StorageSnapshot& snapshot = pending.latest;
    constexpr double NO_READING = std::numeric_limits<double>::quiet_NaN();

    // Adopt the staged series: map nodes spliced in and names moved in, without allocating.
    while (!pending.newRead.empty())
    {
        m_DiskReadHistory.insert(pending.newRead.extract(pending.newRead.begin()));
    }
    while (!pending.newWrite.empty())
    {
        m_DiskWriteHistory.insert(pending.newWrite.extract(pending.newWrite.begin()));
    }
    while (!pending.newLastSeen.empty())
    {
        m_DiskLastSeenSeconds.insert(pending.newLastSeen.extract(pending.newLastSeen.begin()));
    }
    for (auto& name : pending.newOrder)
    {
        m_DiskOrder.push_back(std::move(name)); // insertion order: the display order
    }

    m_Timestamps.push(nowSeconds);
    m_TotalReadHistory.push(totalRateOrNaN(snapshot, &StorageSnapshot::totalReadBytesPerSec));
    m_TotalWriteHistory.push(totalRateOrNaN(snapshot, &StorageSnapshot::totalWriteBytesPerSec));

    // Per-disk history: this sample's rate for each disk present (a disk without measured rates yet
    // is a gap, not a false 0 B/s, #1102), and a NaN placeholder for each known disk absent from it,
    // so every series stays index-aligned with m_Timestamps. NaN, not 0: nothing was measured (#1015).
    for (const DiskSnapshot& disk : snapshot.disks)
    {
        const auto read = m_DiskReadHistory.find(disk.deviceName);
        const auto write = m_DiskWriteHistory.find(disk.deviceName);
        if (read != m_DiskReadHistory.end() && write != m_DiskWriteHistory.end())
        {
            read->second.push(disk.hasRates ? disk.readBytesPerSec : NO_READING);
            write->second.push(disk.hasRates ? disk.writeBytesPerSec : NO_READING);
        }
        if (const auto seen = m_DiskLastSeenSeconds.find(disk.deviceName); seen != m_DiskLastSeenSeconds.end())
        {
            seen->second = nowSeconds;
        }
    }
    // Both maps always have the same key set (they are staged and adopted together).
    for (auto& [name, read] : m_DiskReadHistory)
    {
        if (!pending.present.contains(name))
        {
            read.push(NO_READING);
            if (const auto write = m_DiskWriteHistory.find(name); write != m_DiskWriteHistory.end())
            {
                write->second.push(NO_READING);
            }
        }
    }

    // The rate state and snapshot of this sample, swapped and moved in: nothing here allocates.
    m_DiskStates.swap(pending.diskStates);
    static_assert(std::is_nothrow_move_assignable_v<StorageSnapshot>);
    m_LatestSnapshot = std::move(pending.latest); // last: `snapshot` refers to it

    pruneAbsentDisks(nowSeconds);
    trimHistory(nowSeconds);
}

void StorageModel::pruneAbsentDisks(double nowSeconds) noexcept
{
    // Prune disks absent for longer than the configured history window: by that point their
    // histories hold nothing but NaN padding, so removing the entry changes nothing observable, but
    // retaining it forever would grow m_DiskStates/m_DiskReadHistory/m_DiskWriteHistory/m_DiskOrder
    // without bound on a machine with churning removable/USB storage (#777). Matches trimHistory()'s
    // own wall-clock cutoff. m_DiskOrder must stay in lockstep with the history maps -- publish()
    // indexes them with .at(), which would throw if a name survived in m_DiskOrder after being erased
    // from the maps. Erases in place while iterating m_DiskLastSeenSeconds rather than collecting
    // stale names into a scratch vector first: that allocation could throw right under the memory
    // pressure this pruning exists to relieve, and this runs inside the no-throw commit.
    bool anyDiskPruned = false;
    for (auto it = m_DiskLastSeenSeconds.begin(); it != m_DiskLastSeenSeconds.end();)
    {
        if ((nowSeconds - it->second) > m_MaxHistorySeconds)
        {
            m_DiskReadHistory.erase(it->first);
            m_DiskWriteHistory.erase(it->first);
            m_DiskStates.erase(it->first);
            it = m_DiskLastSeenSeconds.erase(it);
            anyDiskPruned = true;
        }
        else
        {
            ++it;
        }
    }
    if (anyDiskPruned)
    {
        // Single O(N) pass over m_DiskOrder instead of an O(N) std::erase() per stale disk in the
        // loop above, which made the whole prune step O(N*M) for M stale disks against an
        // m_DiskOrder of size N. m_DiskReadHistory has already had the stale names removed above,
        // so "no longer present there" is precisely the prune condition.
        std::erase_if(m_DiskOrder, [this](const std::string& name) { return !m_DiskReadHistory.contains(name); });
    }
}

std::shared_ptr<const StoragePublication> StorageModel::publication() const noexcept
{
    return m_Publication.load();
}

std::uint64_t StorageModel::publicationVersion() const noexcept
{
    return m_Publication.version();
}

void StorageModel::publish()
{
    // Build contents first, commit validity keys last: the version comes from a local candidate and
    // m_PublicationVersion only advances once the generation is committed, so a throw while building
    // (std::bad_alloc from the snapshot copy or the per-disk list) leaves the published generation,
    // its version and m_PublicationVersion consistent. The histories are shared, not copied (#1412):
    // each series is one view of its append-only buffer, so this is O(series) whatever the history
    // length. It runs under a shared lock: latestSnapshot() still reads alongside, and
    // publication() doesn't take m_Mutex at all, so no reader waits for it (#868). Nothing else can
    // write this state meanwhile; the caller holds m_WriterMutex.
    auto publication = std::make_shared<StoragePublication>();
    {
        const std::shared_lock stateLock(m_Mutex);
        publication->version = m_PublicationVersion + 1;
        publication->snapshot = m_LatestSnapshot;
        publication->timestamps = m_Timestamps.view();
        publication->totalReadHistory = m_TotalReadHistory.view();
        publication->totalWriteHistory = m_TotalWriteHistory.view();
        publication->perDiskHistory.reserve(m_DiskOrder.size());
        for (const auto& name : m_DiskOrder)
        {
            publication->perDiskHistory.push_back({
                .deviceName = name,
                .readBytesPerSec = m_DiskReadHistory.at(name).view(),
                .writeBytesPerSec = m_DiskWriteHistory.at(name).view(),
            });
        }
    }
    const std::uint64_t version = publication->version;
    m_Publication.commit(std::move(publication));
    m_PublicationVersion = version;
}

DiskSnapshot
StorageModel::computeDiskSnapshot(const Platform::DiskCounters& current, DiskState& state, const std::chrono::steady_clock::time_point now)
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

    // Compute deltas. Uses `now` (the timestamp sampleAt() captured once, before this cycle's
    // probe read, and the same value it stores into state.prevTime below for next cycle's
    // baseline) rather than a fresh steady_clock::now() call here. A fresh call here would
    // measure elapsed time from a *pre-probe* timestamp (last cycle's) to a *post-probe*
    // timestamp (this cycle's, after m_Probe->read() and any earlier disks in this same loop
    // already ran) -- an inconsistent basis that inflates deltaTime by this cycle's probe
    // latency, depressing every computed rate, and that also skews between disks in the same
    // sample() call, since it'd advance a little further for each disk processed. Using `now`
    // consistently for both sides means every cycle measures the same "start of sampleAt" to
    // "start of next sampleAt" gap, and every disk in one sample shares the identical basis.
    const auto deltaTime = now - state.prevTime;
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
    const double readBytesPerSec = (Numeric::toDouble(deltaReadSectors) * Numeric::toDouble(current.sectorSize)) / deltaSeconds;
    const double writeBytesPerSec = (Numeric::toDouble(deltaWriteSectors) * Numeric::toDouble(current.sectorSize)) / deltaSeconds;
    if (readBytesPerSec > Sampling::MAX_SANE_DISK_RATE_BPS || writeBytesPerSec > Sampling::MAX_SANE_DISK_RATE_BPS)
    {
        // A counter glitch (a reinitialised or re-registered device counter), not I/O: the sample
        // has no rates, so it reads 0 and its history records a gap instead of a spike that would
        // blow out the chart's scale (#1291). Flagged so the system Total is a gap too.
        snap.ratesRejected = true;
        return snap;
    }
    snap.hasRates = true;
    snap.readBytesPerSec = readBytesPerSec;
    snap.writeBytesPerSec = writeBytesPerSec;
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

void StorageModel::trimHistory(double nowSeconds) noexcept
{
    // Drop entries older than the configured time window, except the newest of them while a newer
    // sample remains (see HistoryUtils::discardBefore, #1016). All series are pushed in lockstep with
    // m_Timestamps, so a single discard count keeps them aligned. discardFront is O(1): no copies,
    // rebuilds, or allocations.
    const double cutoff = nowSeconds - m_MaxHistorySeconds;
    const std::size_t removeCount = HistoryUtils::discardBefore(m_Timestamps, cutoff, m_TotalReadHistory, m_TotalWriteHistory);
    for (auto& [name, history] : m_DiskReadHistory)
    {
        history.discardFront(removeCount);
    }
    for (auto& [name, history] : m_DiskWriteHistory)
    {
        history.discardFront(removeCount);
    }
}

StorageSnapshot StorageModel::latestSnapshot() const
{
    std::shared_lock lock(m_Mutex); // NOLINT(misc-const-correctness) - lock guard pattern
    return m_LatestSnapshot;
}

void StorageModel::setMaxHistorySeconds(double seconds)
{
    const std::scoped_lock writerLock(m_WriterMutex);
    {
        const std::unique_lock lock(m_Mutex);
        // The same guardrail as SystemModel and GPUModel, so every model keeps the same window (#1145).
        m_MaxHistorySeconds = Sampling::clampHistorySeconds(seconds);
        applyHistoryCapacity();

        if (!m_Timestamps.empty())
        {
            trimHistory(m_Timestamps.latest());
        }
    }
    // Republish the trimmed history now rather than at the next sample (#1145); see
    // SystemModel::setMaxHistorySeconds(). Nothing is published before the first sample.
    if (m_PublicationVersion != 0)
    {
        publish();
    }
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
