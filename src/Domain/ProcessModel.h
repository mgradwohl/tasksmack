#pragma once

#include "Platform/IProcessProbe.h"
#include "ProcessSnapshot.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
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

    // Aggregated system-level histories derived from per-process data
    [[nodiscard]] std::vector<double> systemNetSentHistory() const;
    [[nodiscard]] std::vector<double> systemNetRecvHistory() const;
    [[nodiscard]] std::vector<double> systemPageFaultsHistory() const;
    [[nodiscard]] std::vector<double> systemThreadCountHistory() const;
    [[nodiscard]] std::vector<double> systemPowerHistory() const;
    [[nodiscard]] std::vector<double> historyTimestamps() const;

    void setMaxHistorySeconds(double seconds);

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

    // ==========================================================================
    // Network Rate Baseline Tracking
    // ==========================================================================
    //
    // Problem: Windows TCP EStats (GetPerTcpConnectionEStats) returns cumulative
    // byte counters *per TCP connection*. When we aggregate these across all
    // connections for a process, the sum can jump wildly because:
    //   1. New connections appear with large cumulative values (data transferred
    //      before we started monitoring)
    //   2. Old connections disappear, removing their contribution
    //   3. The net effect is massive, impossible rate spikes (e.g., 14 EB/s)
    //
    // Solution: Track a "baseline" for each process - the network counter values
    // when we first saw that process. Then compute:
    //   rate = (currentCounters - baselineCounters) / timeSinceFirstSeen
    //
    // This gives us the average bytes/sec since we started monitoring, which:
    //   - Absorbs the initial cumulative values into the baseline
    //   - Smoothly reflects ongoing activity without wild spikes
    //   - Is robust to connection churn (new connections just add to the total)
    //
    // Tradeoff: This is an *average* rate, not instantaneous. A process that was
    // busy 5 minutes ago but idle now will still show some activity. For most
    // monitoring use cases, this is more useful than noisy instantaneous rates.
    //
    // TODO: Investigate better approaches:
    //   - ETW (Event Tracing for Windows) kernel providers for real-time network events
    //   - Track per-connection state to handle connection lifecycle properly
    //   - Use system-wide network interface counters instead (more reliable but less granular)
    //   - On Linux, consider eBPF or netlink INET_DIAG for accurate per-process tracking
    //
    // ==========================================================================
    struct NetworkBaseline
    {
        std::uint64_t netSentBytes = 0;
        std::uint64_t netReceivedBytes = 0;
        std::chrono::steady_clock::time_point firstSeenTime;
    };
    std::unordered_map<std::uint64_t, NetworkBaseline> m_NetworkBaselines;

    std::uint64_t m_PrevTotalCpuTime = 0;
    std::uint64_t m_SystemTotalMemory = 0;                  // For memoryPercent calculation
    long m_TicksPerSecond = 100;                            // For cpuTimeSeconds calculation
    std::chrono::steady_clock::time_point m_PrevSampleTime; // For rate calculations (network, I/O, power)
    bool m_HasPrevSampleTime = false;
    std::chrono::steady_clock::time_point m_StartTime; // For history timestamp alignment
    bool m_HasStartTime = false;

    // Aggregated system histories (aligned by timestamps)
    std::deque<double> m_SystemNetSentHistory;
    std::deque<double> m_SystemNetRecvHistory;
    std::deque<double> m_SystemPageFaultsHistory;
    std::deque<double> m_SystemThreadCountHistory;
    std::deque<double> m_SystemPowerHistory;
    std::deque<double> m_Timestamps;
    double m_MaxHistorySeconds = 300.0; // Align with Storage/System defaults

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
                                                  double elapsedSeconds,
                                                  std::uint64_t timeDeltaUs) const;

    void trimHistory();

    [[nodiscard]] static std::uint64_t makeUniqueKey(std::int32_t pid, std::uint64_t startTime);
    [[nodiscard]] static std::string translateState(char rawState);
};

} // namespace Domain
