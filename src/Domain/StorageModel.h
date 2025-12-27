#pragma once

#include "Domain/StorageSnapshot.h"
#include "Platform/IDiskProbe.h"

#include <chrono>
#include <deque>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Domain
{

/// Manages disk/storage metrics: samples probe, computes rates, maintains history.
/// Thread-safe (allows background sampling + UI reads).
class StorageModel
{
  public:
    explicit StorageModel(std::unique_ptr<Platform::IDiskProbe> probe);
    ~StorageModel() = default;

    StorageModel(const StorageModel&) = delete;
    StorageModel& operator=(const StorageModel&) = delete;
    StorageModel(StorageModel&&) = delete;
    StorageModel& operator=(StorageModel&&) = delete;

    /// Sample the probe and compute new snapshot (call from background thread).
    void sample();

    /// Get the latest snapshot (thread-safe, called from UI thread).
    [[nodiscard]] StorageSnapshot latestSnapshot() const;

    /// Get historical snapshots for graphing (thread-safe).
    /// Returns snapshots in chronological order (oldest first).
    [[nodiscard]] std::vector<StorageSnapshot> history() const;

    // System-level history helpers (aligned to timestamps)
    [[nodiscard]] std::vector<double> totalReadHistory() const;
    [[nodiscard]] std::vector<double> totalWriteHistory() const;
    [[nodiscard]] std::vector<double> historyTimestamps() const;

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
    };

    DiskSnapshot computeDiskSnapshot(const Platform::DiskCounters& current, DiskState& state);
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

    double m_MaxHistorySeconds = 300.0; // 5 minutes default
};

} // namespace Domain
