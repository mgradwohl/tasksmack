#pragma once

#include "Domain/StorageSnapshot.h"
#include "History.h"
#include "ISamplable.h"
#include "Platform/IDiskProbe.h"

#include <atomic>
#include <chrono>
#include <memory>
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
/// Thread-safe (allows background sampling + UI reads).
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
    /// was absent (disappeared or not yet seen) are represented as 0.0.
    [[nodiscard]] std::vector<PerDiskHistory> perDiskHistory() const;
    [[nodiscard]] std::shared_ptr<const StoragePublication> publication() const noexcept;
    [[nodiscard]] std::uint64_t publicationVersion() const noexcept;

    /// Configure history retention.
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

    static DiskSnapshot computeDiskSnapshot(const Platform::DiskCounters& current, DiskState& state);
    void trimHistory(double nowSeconds);
    void applyHistoryCapacity();

    std::unique_ptr<Platform::IDiskProbe> m_Probe;

    mutable std::shared_mutex m_Mutex;
    StorageSnapshot m_LatestSnapshot;
    HistoryBuffer<StorageSnapshot> m_History;
    HistoryBuffer<double> m_Timestamps; // Seconds since start

    // Per-device state for delta calculations
    std::unordered_map<std::string, DiskState> m_DiskStates;
    std::chrono::steady_clock::time_point m_PrevSampleTime;
    std::chrono::steady_clock::time_point m_StartTime;
    bool m_HasPrevSample = false;

    // Per-device I/O history for per-disk charting. Newly discovered disks are
    // backfilled with zeros (clamped to ring capacity) and absent disks receive
    // 0.0 placeholders, so every series stays index-aligned with m_Timestamps.
    std::unordered_map<std::string, HistoryBuffer<double>> m_DiskReadHistory;
    std::unordered_map<std::string, HistoryBuffer<double>> m_DiskWriteHistory;
    std::vector<std::string> m_DiskOrder; ///< Insertion-order disk names for consistent display
    // Wall-clock time (nowSeconds, same clock as m_Timestamps) each device name was last seen
    // in a live sample, so a name absent for longer than the configured history window (at
    // which point its histories hold nothing but zero padding) can be pruned instead of
    // retained forever -- otherwise a machine with churning removable/USB storage leaks one
    // entry per distinct device name ever seen, across
    // m_DiskStates/m_DiskReadHistory/m_DiskWriteHistory/m_DiskOrder (#777). Deliberately
    // time-based, matching trimHistory()'s own cutoff, rather than counting sample() calls:
    // historyCapacityForSeconds() sizes ring buffers for the fastest *supported* refresh
    // cadence, not the actual one, so a call-count threshold could retain stale entries far
    // longer than m_MaxHistorySeconds at any slower cadence.
    std::unordered_map<std::string, double> m_DiskLastSeenSeconds;
    std::shared_ptr<const StoragePublication> m_Publication = std::make_shared<const StoragePublication>();
    std::uint64_t m_PublicationVersion = 0;
    std::atomic<std::uint64_t> m_PublishedPublicationVersion{0};

    double m_MaxHistorySeconds = 300.0; // 5 minutes default

    void publish();
};

} // namespace Domain
