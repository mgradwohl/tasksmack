#pragma once

#include "Domain/StorageSnapshot.h"
#include "ISamplable.h"
#include "Platform/IDiskProbe.h"

#include <atomic>
#include <chrono>
#include <deque>
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

    std::unique_ptr<Platform::IDiskProbe> m_Probe;

    mutable std::shared_mutex m_Mutex;
    StorageSnapshot m_LatestSnapshot;
    std::deque<StorageSnapshot> m_History;
    std::deque<double> m_Timestamps; // Seconds since start

    // Per-device state for delta calculations
    std::unordered_map<std::string, DiskState> m_DiskStates;
    std::chrono::steady_clock::time_point m_PrevSampleTime;
    std::chrono::steady_clock::time_point m_StartTime;
    bool m_HasPrevSample = false;

    // Per-device I/O history for per-disk charting (deques aligned to m_Timestamps)
    std::unordered_map<std::string, std::deque<double>> m_DiskReadHistory;
    std::unordered_map<std::string, std::deque<double>> m_DiskWriteHistory;
    std::vector<std::string> m_DiskOrder; ///< Insertion-order disk names for consistent display
    std::shared_ptr<const StoragePublication> m_Publication = std::make_shared<const StoragePublication>();
    std::uint64_t m_PublicationVersion = 0;
    std::atomic<std::uint64_t> m_PublishedPublicationVersion{0};

    double m_MaxHistorySeconds = 300.0; // 5 minutes default

    void publish();
};

} // namespace Domain
