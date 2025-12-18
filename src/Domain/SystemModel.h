#pragma once

#include "History.h"
#include "Platform/ISystemProbe.h"
#include "SystemSnapshot.h"

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

    /// Get latest computed snapshot (copy for thread safety).
    [[nodiscard]] SystemSnapshot snapshot() const;

    /// What the underlying probe supports.
    [[nodiscard]] const Platform::SystemCapabilities& capabilities() const;

    // History access (read-only copies)
    static constexpr size_t HISTORY_SIZE = 120; // 2 minutes at 1 Hz

    [[nodiscard]] std::vector<float> cpuHistory() const;
    [[nodiscard]] std::vector<float> cpuUserHistory() const;
    [[nodiscard]] std::vector<float> cpuSystemHistory() const;
    [[nodiscard]] std::vector<float> cpuIowaitHistory() const;
    [[nodiscard]] std::vector<float> cpuIdleHistory() const;
    [[nodiscard]] std::vector<float> memoryHistory() const;
    [[nodiscard]] std::vector<float> swapHistory() const;
    [[nodiscard]] std::vector<std::vector<float>> perCoreHistory() const;

  private:
    std::unique_ptr<Platform::ISystemProbe> m_Probe;
    Platform::SystemCapabilities m_Capabilities;

    // Previous counters for delta calculation
    Platform::SystemCounters m_PrevCounters;
    bool m_HasPrevious = false;

    // Latest computed snapshot
    SystemSnapshot m_Snapshot;

    // History buffers
    History<float, HISTORY_SIZE> m_CpuHistory;
    History<float, HISTORY_SIZE> m_CpuUserHistory;
    History<float, HISTORY_SIZE> m_CpuSystemHistory;
    History<float, HISTORY_SIZE> m_CpuIowaitHistory;
    History<float, HISTORY_SIZE> m_CpuIdleHistory;
    History<float, HISTORY_SIZE> m_MemoryHistory;
    History<float, HISTORY_SIZE> m_SwapHistory;
    std::vector<History<float, HISTORY_SIZE>> m_PerCoreHistory;

    // Thread safety
    mutable std::shared_mutex m_Mutex;

    // Helpers
    void computeSnapshot(const Platform::SystemCounters& counters);
    [[nodiscard]] static CpuUsage computeCpuUsage(const Platform::CpuCounters& current, const Platform::CpuCounters& previous);
};

} // namespace Domain
