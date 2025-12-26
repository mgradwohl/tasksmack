#pragma once

#include "Platform/IProcessProbe.h"
#include "ProcessSnapshot.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace Domain
{

/// Owns a process probe, caches previous counters, and computes CPU% deltas.
/// Call refresh() periodically; snapshots() returns the latest computed data.
/// Thread-safe: can receive updates from background sampler.
class ProcessModel
{
  public:
    explicit ProcessModel(std::unique_ptr<Platform::IProcessProbe> probe);
    ~ProcessModel() = default;

    ProcessModel(const ProcessModel&) = delete;
    ProcessModel& operator=(const ProcessModel&) = delete;
    ProcessModel(ProcessModel&&) = delete;
    ProcessModel& operator=(ProcessModel&&) = delete;

    /// Refresh process data from the probe and compute new snapshots.
    /// Thread-safe.
    void refresh();

    /// Update with externally-provided counters (for background sampler).
    /// Thread-safe.
    void updateFromCounters(const std::vector<Platform::ProcessCounters>& counters, std::uint64_t totalCpuTime);

    /// Get latest computed snapshots (copy for thread safety).
    [[nodiscard]] std::vector<ProcessSnapshot> snapshots() const;

    /// Number of processes in latest snapshot.
    [[nodiscard]] std::size_t processCount() const;

    /// What the underlying probe supports.
    [[nodiscard]] const Platform::ProcessCapabilities& capabilities() const;

  private:
    std::unique_ptr<Platform::IProcessProbe> m_Probe;
    Platform::ProcessCapabilities m_Capabilities;

    // Previous counters for delta calculation (keyed by uniqueKey)
    std::unordered_map<std::uint64_t, Platform::ProcessCounters> m_PrevCounters;
    // Peak RSS tracking (keyed by uniqueKey)
    std::unordered_map<std::uint64_t, std::uint64_t> m_PeakRss;
    std::uint64_t m_PrevTotalCpuTime = 0;
    std::uint64_t m_SystemTotalMemory = 0; // For memoryPercent calculation
    long m_TicksPerSecond = 100;           // For cpuTimeSeconds calculation

    // Time tracking for rate calculations (network, I/O)
    std::chrono::steady_clock::time_point m_PrevSampleTime;
    bool m_HasPrevSampleTime = false;

    // Latest computed snapshots
    std::vector<ProcessSnapshot> m_Snapshots;

    // Thread safety
    mutable std::shared_mutex m_Mutex;

    // Helpers
    void computeSnapshots(const std::vector<Platform::ProcessCounters>& counters, std::uint64_t totalCpuTime);

    [[nodiscard]] ProcessSnapshot computeSnapshot(const Platform::ProcessCounters& current,
                                                  const Platform::ProcessCounters* previous,
                                                  std::uint64_t totalCpuDelta,
                                                  std::uint64_t systemTotalMemory,
                                                  long ticksPerSecond,
                                                  double timeDeltaSeconds) const;

    [[nodiscard]] static std::uint64_t makeUniqueKey(std::int32_t pid, std::uint64_t startTime);
    [[nodiscard]] static std::string translateState(char rawState);
};

} // namespace Domain
