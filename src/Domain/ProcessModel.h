#pragma once

#include "History.h"
#include "ISamplable.h"
#include "Platform/IProcessProbe.h"
#include "ProcessSnapshot.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace Domain
{

// Forward declaration
class GPUModel;

struct ProcessSystemHistories
{
    std::uint64_t version = 0;
    std::vector<double> timestamps;
    std::vector<double> power;
    std::vector<double> pageFaults;
    std::vector<double> threadCount;
    std::vector<double> handleCount;
};

/// Owns a process probe, caches previous counters, and computes CPU% deltas.
/// Call refresh() periodically; snapshots() returns the latest computed data.
/// Thread-safe: can receive updates from background sampler.
class ProcessModel : public ISamplable
{
  public:
    using Clock = std::chrono::steady_clock;
    using NowFunction = std::function<Clock::time_point()>;

    explicit ProcessModel(std::unique_ptr<Platform::IProcessProbe> probe, NowFunction now = [] { return Clock::now(); });
    ~ProcessModel() override = default;

    ProcessModel(const ProcessModel&) = delete;
    ProcessModel& operator=(const ProcessModel&) = delete;
    ProcessModel(ProcessModel&&) = delete;
    ProcessModel& operator=(ProcessModel&&) = delete;

    /// Perform one sampling iteration (ISamplable implementation).
    void sample() override
    {
        refresh();
    }

    /// Refresh process data from the probe and compute new snapshots.
    /// Thread-safe.
    void refresh();

    /// Update with externally-provided counters (for background sampler).
    /// Thread-safe.
    void updateFromCounters(const std::vector<Platform::ProcessCounters>& counters, std::uint64_t totalCpuTime);

    /// Get latest computed snapshots (copy for thread safety).
    [[nodiscard]] std::vector<ProcessSnapshot> snapshots() const;

    /// Find a single snapshot by PID without copying the full snapshot vector.
    /// Always reflects the latest published data, unlike a version-gated render cache.
    [[nodiscard]] std::optional<ProcessSnapshot> findSnapshot(std::int32_t pid) const;

    /// Copy snapshots only when a newer version exists.
    /// Returns true and copies data when an update is available; otherwise returns false.
    [[nodiscard]] bool
    tryCopySnapshotsIfNewer(std::uint64_t lastSeenVersion, std::vector<ProcessSnapshot>& outSnapshots, std::uint64_t& outVersion) const;

    /// Monotonically increasing counter, incremented each time snapshots are updated.
    /// UI can compare against a cached value to skip redundant copies when data hasn't changed.
    [[nodiscard]] std::uint64_t snapshotVersion() const;
    [[nodiscard]] bool tryCopySystemHistoriesIfNewer(std::uint64_t lastSeenVersion, ProcessSystemHistories& outHistories) const;

    // Aggregated system-level histories derived from per-process data
    [[nodiscard]] std::vector<double> systemNetSentHistory() const;
    [[nodiscard]] std::vector<double> systemNetRecvHistory() const;
    [[nodiscard]] std::vector<double> systemPageFaultsHistory() const;
    [[nodiscard]] std::vector<double> systemThreadCountHistory() const;
    [[nodiscard]] std::vector<double> systemHandleCountHistory() const;
    [[nodiscard]] std::vector<double> systemPowerHistory() const;
    [[nodiscard]] std::vector<double> historyTimestamps() const;

    void setMaxHistorySeconds(double seconds);

    /// Number of processes in latest snapshot.
    [[nodiscard]] std::size_t processCount() const;

    /// What the underlying probe supports.
    [[nodiscard]] const Platform::ProcessCapabilities& capabilities() const;

    /// Set GPU model for per-process GPU data.
    /// When set, refresh() automatically queries GPU counters and merges them.
    void setGPUModel(std::shared_ptr<GPUModel> gpuModel);

    /// Hint whether interactive resize/move redraw is currently active.
    /// Allows throttling expensive GPU merge work during interactions.
    void setInteractionActive(bool active) noexcept;

  private:
    std::unique_ptr<Platform::IProcessProbe> m_Probe;
    NowFunction m_Now;
    std::shared_ptr<GPUModel> m_GPUModel; // For per-process GPU data
    Platform::ProcessCapabilities m_Capabilities;

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
    // Future improvement options (would require significant platform work):
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

    // Per-process tracking state.  Consolidating previous counters, network
    // baseline, and peak-RSS into one struct reduces per-process map lookups
    // in computeSnapshots() from 3-4 separate finds/inserts to a single
    // try_emplace, improving cache locality and reducing map overhead.
    struct PerProcessState
    {
        Platform::ProcessCounters counters{}; // counters from last refresh (for delta)
        NetworkBaseline networkBaseline{};    // network rate baseline (see above)
        std::uint64_t peakRss = 0;            // tracked peak RSS
        std::uint64_t generation = 0;         // refresh generation when last seen
    };

    // Single map replaces m_PrevCounters + m_NetworkBaselines + m_PeakRss + m_ActiveKeys.
    std::unordered_map<std::uint64_t, PerProcessState> m_PerProcessState;
    // Monotonically increasing counter; bumped each computeSnapshots() call.
    // Entries with generation != m_CurrentGeneration belong to dead processes.
    std::uint64_t m_CurrentGeneration = 0;

    std::uint64_t m_PrevTotalCpuTime = 0;
    std::uint64_t m_SystemTotalMemory = 0; // For memoryPercent calculation
    long m_TicksPerSecond = 100;           // For cpuTimeSeconds calculation
    Clock::time_point m_PrevSampleTime;    // For rate calculations (network, I/O, power)
    bool m_HasPrevSampleTime = false;
    Clock::time_point m_StartTime; // For history timestamp alignment
    bool m_HasStartTime = false;

    // Aggregated system histories (aligned by timestamps). Capacity is set from the
    // configured window; time-based trimming via discardFront keeps them in the window.
    HistoryBuffer<double> m_SystemNetSentHistory;
    HistoryBuffer<double> m_SystemNetRecvHistory;
    HistoryBuffer<double> m_SystemPageFaultsHistory;
    HistoryBuffer<double> m_SystemThreadCountHistory;
    HistoryBuffer<double> m_SystemHandleCountHistory;
    HistoryBuffer<double> m_SystemPowerHistory;
    HistoryBuffer<double> m_Timestamps;
    double m_MaxHistorySeconds = 300.0; // Align with Storage/System defaults

    // Latest computed snapshots
    std::vector<ProcessSnapshot> m_Snapshots;
    std::uint64_t m_SnapshotVersion = 0;
    std::atomic<std::uint64_t> m_PublishedSnapshotVersion{0};
    std::atomic<bool> m_InteractionActive{false};
    Clock::time_point m_LastGpuMergeTime;
    bool m_HasLastGpuMergeTime = false;

    // Thread safety
    mutable std::shared_mutex m_Mutex;
    std::mutex m_SamplingMutex;

    // Helpers
    void computeSnapshots(const std::vector<Platform::ProcessCounters>& counters, std::uint64_t totalCpuTime);

    static void mergeGPUData(std::vector<ProcessSnapshot>& snapshots, const std::shared_ptr<GPUModel>& gpuModel);

    [[nodiscard]] static ProcessSnapshot computeSnapshot(const Platform::ProcessCounters& current,
                                                         const Platform::ProcessCounters* previous,
                                                         std::uint64_t totalCpuDelta,
                                                         std::uint64_t systemTotalMemory,
                                                         long ticksPerSecond,
                                                         double elapsedSeconds,
                                                         std::uint64_t timeDeltaUs);

    void trimHistory();
    void applyHistoryCapacity();

    [[nodiscard]] static std::uint64_t makeUniqueKey(std::int32_t pid, std::uint64_t startTime);
    [[nodiscard]] static std::string translateState(char rawState);
};

} // namespace Domain
