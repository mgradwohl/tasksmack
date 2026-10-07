#pragma once

#include "Domain/StorageSnapshot.h"
#include "History.h"
#include "ISamplable.h"
#include "Platform/IDiskProbe.h"
#include "PublicationSlot.h"
#include "SamplingConfig.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Domain
{

/// Per-device I/O history for charting.
struct PerDiskHistory
{
    std::string deviceName;
    std::vector<double> readBytesPerSec;  ///< Aligned to StorageModel::historyTimestamps()
    std::vector<double> writeBytesPerSec; ///< Aligned to StorageModel::historyTimestamps()
};

struct StoragePublication
{
    std::uint64_t version = 0;
    StorageSnapshot snapshot;
    std::vector<double> timestamps;
    std::vector<double> totalReadHistory;
    std::vector<double> totalWriteHistory;
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

    /// Per-device I/O history for charting individual disks.
    /// Each entry is strictly aligned to historyTimestamps(): every per-disk
    /// vector has the same length as historyTimestamps(). Samples where a disk
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

    static DiskSnapshot
    computeDiskSnapshot(const Platform::DiskCounters& current, DiskState& state, std::chrono::steady_clock::time_point now);
    /// One sample from @p counters: the shared body of sampleAt() and sampleSeries(). Publishes it
    /// when @p publishNow. Requires m_WriterMutex held and m_Mutex not held.
    void applySample(const Platform::SystemDiskCounters& counters,
                     const Platform::DiskCapabilities& caps,
                     std::chrono::steady_clock::time_point now,
                     bool publishNow);
    void trimHistory(double nowSeconds);
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
    HistoryBuffer<StorageSnapshot> m_History;
    HistoryBuffer<double> m_Timestamps; // Seconds since start

    // Per-device state for delta calculations
    std::unordered_map<std::string, DiskState> m_DiskStates;

    // Per-device I/O history for per-disk charting. Newly discovered disks are
    // backfilled with NaN (clamped to ring capacity) and absent disks receive NaN
    // placeholders, so every series stays index-aligned with m_Timestamps and a
    // sample where nothing was measured is a gap, not a false zero (#1015).
    std::unordered_map<std::string, HistoryBuffer<double>> m_DiskReadHistory;
    std::unordered_map<std::string, HistoryBuffer<double>> m_DiskWriteHistory;
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
    /// Requires m_WriterMutex held and m_Mutex not held.
    void publish();
};

} // namespace Domain
