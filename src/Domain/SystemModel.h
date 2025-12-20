#pragma once

#include "History.h"
#include "Platform/ISystemProbe.h"
#include "SystemSnapshot.h"

#include <deque>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace Domain
{

/// Owns a system probe, caches previous counters, and computes CPU% deltas.
/// Call refresh() periodically; snapshot() returns the latest computed data.
/// Thread-safe: can receive updates from background sampler.
class SystemModel
{
  public:
    explicit SystemModel(std::unique_ptr<Platform::ISystemProbe> probe);
    ~SystemModel() = default;

    SystemModel(const SystemModel&) = delete;
    SystemModel& operator=(const SystemModel&) = delete;
    SystemModel(SystemModel&&) = delete;
    SystemModel& operator=(SystemModel&&) = delete;

    /// Refresh system data from the probe and compute new snapshot.
    /// Thread-safe.
    void refresh();

    /// Update with externally-provided counters (for background sampler).
    /// Thread-safe.
    void updateFromCounters(const Platform::SystemCounters& counters);
    void updateFromCounters(const Platform::SystemCounters& counters, double nowSeconds);

    /// Get latest computed snapshot (copy for thread safety).
    [[nodiscard]] SystemSnapshot snapshot() const;

    /// What the underlying probe supports.
    [[nodiscard]] const Platform::SystemCapabilities& capabilities() const;

    /// Configure maximum retained history duration (seconds).
    void setMaxHistorySeconds(double seconds);
    [[nodiscard]] double maxHistorySeconds() const
    {
        return m_MaxHistorySeconds;
    }

    // History access (read-only copies)

    [[nodiscard]] std::vector<float> cpuHistory() const;
    [[nodiscard]] std::vector<float> cpuUserHistory() const;
    [[nodiscard]] std::vector<float> cpuSystemHistory() const;
    [[nodiscard]] std::vector<float> cpuIowaitHistory() const;
    [[nodiscard]] std::vector<float> cpuIdleHistory() const;
    [[nodiscard]] std::vector<float> memoryHistory() const;
    [[nodiscard]] std::vector<float> swapHistory() const;
    [[nodiscard]] std::vector<float> memoryCachedHistory() const;
    [[nodiscard]] std::vector<std::vector<float>> perCoreHistory() const;
    [[nodiscard]] std::vector<double> timestamps() const;

  private:
    std::unique_ptr<Platform::ISystemProbe> m_Probe;
    Platform::SystemCapabilities m_Capabilities;

    // Previous counters for delta calculation
    Platform::SystemCounters m_PrevCounters;
    bool m_HasPrevious = false;

    // Latest computed snapshot
    SystemSnapshot m_Snapshot;

    // History buffers (trimmed by time window)
    std::deque<float> m_CpuHistory;
    std::deque<float> m_CpuUserHistory;
    std::deque<float> m_CpuSystemHistory;
    std::deque<float> m_CpuIowaitHistory;
    std::deque<float> m_CpuIdleHistory;
    std::deque<float> m_MemoryHistory;
    std::deque<float> m_MemoryCachedHistory;
    std::deque<float> m_SwapHistory;
    std::deque<double> m_Timestamps;
    std::vector<std::deque<float>> m_PerCoreHistory;

    double m_MaxHistorySeconds = 300.0; // Default 5 minutes

    // Thread safety
    mutable std::shared_mutex m_Mutex;

    // Helpers
    void computeSnapshot(const Platform::SystemCounters& counters, double nowSeconds);
    void trimHistory(double nowSeconds);
    [[nodiscard]] static CpuUsage computeCpuUsage(const Platform::CpuCounters& current, const Platform::CpuCounters& previous);
};

} // namespace Domain
