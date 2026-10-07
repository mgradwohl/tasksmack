#pragma once

#include "Domain/StorageSnapshot.h"
#include "History.h"
#include "ISamplable.h"
#include "Platform/IDiskProbe.h"
#include "PublicationSlot.h"
#include "SamplingConfig.h"
#include "SharedHistory.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Domain
{

/// Per-device I/O history for charting: views of the model's shared history (#1412), each one
/// immutable and convertible to std::span<const double>.
struct PerDiskHistory
{
    std::string deviceName;
    HistoryView<double> readBytesPerSec;  ///< Aligned to the timestamps it was published with
    HistoryView<double> writeBytesPerSec; ///< Aligned to the timestamps it was published with
};

/// One immutable generation of the storage model's state. The histories are views of the model's
/// shared history (#1412): a publish shares their samples rather than copying them, so it costs
/// O(series), not O(history x series). Every series is aligned sample for sample with timestamps.
struct StoragePublication
{
    std::uint64_t version = 0;
    StorageSnapshot snapshot;
    HistoryView<double> timestamps;
    HistoryView<double> totalReadHistory;  ///< NaN where the total was not a measurement (#1102, #1291)
    HistoryView<double> totalWriteHistory; ///< Likewise
    std::vector<PerDiskHistory> perDiskHistory;
};

/// Manages disk/storage metrics: samples probe, computes rates, maintains history.
/// Thread-safe (allows background sampling + UI reads). Writers (sampleAt(), setMaxHistorySeconds())
/// are serialised on m_WriterMutex; each builds its publication outside every lock a reader takes
/// and swaps it in through m_Publication, so publication() never waits for a history copy (#868).
class StorageModel : public ISamplable
{
  public:
    explicit StorageModel(std::unique_ptr<Platform::IDiskProbe> probe);
    ~StorageModel() override = default;

    StorageModel(const StorageModel&) = delete;
    StorageModel& operator=(const StorageModel&) = delete;
    StorageModel(StorageModel&&) = delete;
    StorageModel& operator=(StorageModel&&) = delete;

    /// Sample the probe and compute new snapshot (call from background thread).
    void sample() override;

    /// Same as sample(), but with an explicit "now" instead of reading
    /// std::chrono::steady_clock::now(), so time-dependent behavior (e.g. history pruning,
    /// #777) can be tested deterministically without waiting out the real interval. Mirrors
    /// SystemModel's updateFromCounters(counters, nowSeconds) test-injection pattern.
    void sampleAt(std::chrono::steady_clock::time_point now);

    /// Fills in the next sample of a series: its counters and when they were read, returning false
    /// when there are no more.
    using CounterSeriesSource = std::function<bool(Platform::SystemDiskCounters& counters, std::chrono::steady_clock::time_point& now)>;

    /// Applies a series of samples, oldest first, as sampleAt() would one at a time with these
    /// counters in place of the probe's, but with one publish at the end: a history preload (the
    /// synthetic scenario fills the whole window at startup, #1413), which one publish per sample
    /// would make O(N^2). A sample not later than the newest one held is skipped, so the history
    /// stays in time order. Call it from the sampling thread, or before sampling starts.
    void sampleSeries(const CounterSeriesSource& next);

    /// Get the latest snapshot (thread-safe, called from UI thread).
    [[nodiscard]] StorageSnapshot latestSnapshot() const;

    /// Get historical snapshots for graphing (thread-safe).
    /// Returns snapshots in chronological order (oldest first).
    [[nodiscard]] std::vector<StorageSnapshot> history() const;

    // System-level history helpers (aligned to timestamps)
    [[nodiscard]] std::vector<double> totalReadHistory() const;
    [[nodiscard]] std::vector<double> totalWriteHistory() const;
    [[nodiscard]] std::vector<double> historyTimestamps() const;

    /// Per-device I/O history for charting individual disks, as views of the shared history.
    /// Each entry is strictly aligned to historyTimestamps(): every per-disk
    /// series has the same length as historyTimestamps(). Samples where a disk
    /// was absent (disappeared or not yet seen) are NaN: no reading, drawn as a gap.
    [[nodiscard]] std::vector<PerDiskHistory> perDiskHistory() const;
    [[nodiscard]] std::shared_ptr<const StoragePublication> publication() const noexcept;
    [[nodiscard]] std::uint64_t publicationVersion() const noexcept;

    /// Configure history retention, clamped to SamplingConfig's range. Trims the history to the new
    /// window and republishes it at once, once anything has been published (#1145).
    void setMaxHistorySeconds(double seconds);

    /// Get capabilities from the underlying probe.
    [[nodiscard]] Platform::DiskCapabilities capabilities() const;

  private:
    struct DiskState
    {
        std::string deviceName;
        Platform::DiskCounters prevCounters;
        std::chrono::steady_clock::time_point prevTime;
        bool hasPrev = false;
        bool isSeedTransition = false;
    };

    /// One sample's changes, staged: everything it allocates is made here, so applying it cannot
    /// fail part way (#1412).
    struct PendingSample
    {
        StorageSnapshot latest;                                               // the next m_LatestSnapshot
        std::unordered_map<std::string, DiskState> diskStates;                // the next m_DiskStates, swapped in
        std::unordered_set<std::string> present;                              // device names in this sample
        std::vector<char> firstOfName;                                        // per snapshot disk: 1 if the first of its name
        std::unordered_map<std::string, SharedHistoryBuffer<double>> newRead; // new disks, backfilled
        std::unordered_map<std::string, SharedHistoryBuffer<double>> newWrite;
        std::vector<std::string> newOrder; // new disks, in the order they appear
        std::unordered_map<std::string, double> newLastSeen;
    };

    static DiskSnapshot
    computeDiskSnapshot(const Platform::DiskCounters& current, DiskState& state, std::chrono::steady_clock::time_point now);
    /// The snapshot of one sample, advancing the per-disk rate state in @p diskStates.
    static StorageSnapshot computeSnapshot(const Platform::SystemDiskCounters& counters,
                                           const Platform::DiskCapabilities& caps,
                                           std::chrono::steady_clock::time_point now,
                                           std::unordered_map<std::string, DiskState>& diskStates);
    /// One sample from @p counters: the shared body of sampleAt() and sampleSeries(). Publishes it
    /// when @p publishNow. Requires m_WriterMutex held and m_Mutex not held.
    /// Exception guarantee (#1412): the history append is a transaction with the strong guarantee --
    /// a throw (std::bad_alloc) before its commit leaves the history, rate state and snapshot as they
    /// were, every series aligned. After the commit, publish() can still throw while building the
    /// publication; that leaves the history one sample ahead of an unchanged publication and version,
    /// which the next successful publish catches up.
    void applySample(const Platform::SystemDiskCounters& counters,
                     const Platform::DiskCapabilities& caps,
                     std::chrono::steady_clock::time_point now,
                     bool publishNow);
    /// Create and backfill the series of disks new in @p snapshot in @p pending, and reserve room for
    /// one more sample in every existing series and the buckets and slots the append will use. May
    /// throw; changes nothing observable. Requires m_Mutex held exclusively.
    void stageHistoryAppend(PendingSample& pending, const StorageSnapshot& snapshot, double nowSeconds);
    /// Apply a staged sample: adopt the staged series and state, append @p snapshot to every series,
    /// prune long-absent disks and trim. Uses only what stageHistoryAppend() reserved, so it does not
    /// allocate or throw. Requires m_Mutex held exclusively.
    void commitHistoryAppend(PendingSample& pending, StorageSnapshot&& snapshot, double nowSeconds) noexcept;
    /// Forget disks absent for longer than the history window (#777).
    void pruneAbsentDisks(double nowSeconds) noexcept;
    void trimHistory(double nowSeconds) noexcept;
    void applyHistoryCapacity();

    std::unique_ptr<Platform::IDiskProbe> m_Probe;

    // Serialises the writers, sampleAt(), sampleSeries() and setMaxHistorySeconds(), from the counter processing
    // through the publication commit, so generations are numbered and committed in order. Readers
    // never take it. Taken before m_Mutex, never while holding it.
    std::mutex m_WriterMutex;
    // Guards the history state below for the per-field accessors: writers mutate it exclusively,
    // and publish() reads it under a shared lock, so neither publication() nor those accessors wait
    // on a publication's copy.
    mutable std::shared_mutex m_Mutex;
    StorageSnapshot m_LatestSnapshot;
    HistoryBuffer<StorageSnapshot> m_History; // for history(); not published
    // The published series: shared append-only buffers that publish() hands out views of instead of
    // copies (#1412), trimmed by time window in lockstep with m_History.
    SharedHistoryBuffer<double> m_Timestamps;       // Seconds since start
    SharedHistoryBuffer<double> m_TotalReadHistory; // totalRateOrNaN() of each sample
    SharedHistoryBuffer<double> m_TotalWriteHistory;

    // Per-device state for delta calculations
    std::unordered_map<std::string, DiskState> m_DiskStates;

    // Per-device I/O history for per-disk charting. Newly discovered disks are
    // backfilled with NaN (clamped to ring capacity) and absent disks receive NaN
    // placeholders, so every series stays index-aligned with m_Timestamps and a
    // sample where nothing was measured is a gap, not a false zero (#1015).
    std::unordered_map<std::string, SharedHistoryBuffer<double>> m_DiskReadHistory;
    std::unordered_map<std::string, SharedHistoryBuffer<double>> m_DiskWriteHistory;
    std::vector<std::string> m_DiskOrder; ///< Insertion-order disk names for consistent display
    // Wall-clock time (nowSeconds, same clock as m_Timestamps) each device name was last seen
    // in a live sample, so a name absent for longer than the configured history window (at
    // which point its histories hold nothing but NaN padding) can be pruned instead of
    // retained forever -- otherwise a machine with churning removable/USB storage leaks one
    // entry per distinct device name ever seen, across
    // m_DiskStates/m_DiskReadHistory/m_DiskWriteHistory/m_DiskOrder (#777). Deliberately
    // time-based, matching trimHistory()'s own cutoff, rather than counting sample() calls:
    // historyCapacityForSeconds() sizes ring buffers for the fastest *supported* refresh
    // cadence, not the actual one, so a call-count threshold could retain stale entries far
    // longer than m_MaxHistorySeconds at any slower cadence.
    std::unordered_map<std::string, double> m_DiskLastSeenSeconds;
    PublicationSlot<StoragePublication> m_Publication;
    std::uint64_t m_PublicationVersion = 0; // guarded by m_WriterMutex; the last committed generation

    double m_MaxHistorySeconds = Sampling::HISTORY_SECONDS_DEFAULT; // 5 minutes default

    /// Build the next generation from the history state under a shared lock, then commit it.
    /// Strong guarantee: a throw while building leaves the publication and its version unchanged.
    /// Requires m_WriterMutex held and m_Mutex not held.
    void publish();
};

} // namespace Domain
