#pragma once

#include "History.h"
#include "ISamplable.h"
#include "Platform/IProcessProbe.h"
#include "ProcessEnergyAttribution.h"
#include "ProcessSnapshot.h"
#include "SamplingConfig.h"
#include "SocketTrafficAccumulator.h"

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
    // The probe's capabilities as of the latest published generation (#1254): a probe can withdraw
    // one after the first sample (Windows' EStats check, #1161), so they aren't fixed at startup.
    Platform::ProcessCapabilities capabilities;
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

    /// Result of findSnapshotWithVersion(): a snapshot paired with the exact publication
    /// version it was read from.
    struct SnapshotLookupResult
    {
        ProcessSnapshot snapshot;
        std::uint64_t version = 0;
    };

    /// Same lookup as findSnapshot(), but also returns the publication version the snapshot
    /// was read under, both under the same lock. A caller that instead calls findSnapshot()
    /// and snapshotVersion() as two separate calls can have a publish land in between them,
    /// pairing a snapshot from one generation with the version number of another -- e.g. a
    /// consumer using that version to gate "is this new data worth recording" (as
    /// ProcessDetailsPanel's history once did; it now uses watchedSamplesSince()) could see a
    /// version that never actually matched the snapshot content it was given, silently
    /// skipping or duplicating a history point.
    [[nodiscard]] std::optional<SnapshotLookupResult> findSnapshotWithVersion(std::int32_t pid) const;

    /// Copy the snapshot generation only when a newer version exists. "Copy" is a shared_ptr
    /// assignment (an O(1) refcount bump), not a deep copy of the underlying vector: the
    /// vector is immutable once published, so readers can share it directly instead of each
    /// duplicating every process's data under the lock. Returns true and updates outSnapshots
    /// when a newer generation is available; otherwise returns false and leaves outSnapshots
    /// untouched. With @p outCapabilities, the probe's capabilities published with that generation
    /// are copied into it under the same lock (#1254), so a reader keeps them current at no extra cost.
    [[nodiscard]] bool tryCopySnapshotsIfNewer(std::uint64_t lastSeenVersion,
                                               std::shared_ptr<const std::vector<ProcessSnapshot>>& outSnapshots,
                                               std::uint64_t& outVersion,
                                               Platform::ProcessCapabilities* outCapabilities = nullptr) const;

    /// How many of the watched process's samples are kept for watchedSamplesSince(): every generation
    /// for 6.4 s at the fastest refresh interval, so a reader that polls once per UI frame -- 200 ms
    /// apart while minimised -- misses none unless the UI stalls for longer than that.
    static constexpr std::size_t WATCHED_SAMPLE_CAPACITY = 64;

    /// Keep a sample of process @p pid from every generation published from now on, for
    /// watchedSamplesSince() (#1098). Replaces any earlier watch and drops its samples; the process
    /// as the current generation lists it (if one has been published) becomes the first sample, so
    /// a reader needn't wait for the next refresh. pid <= 0 stops watching.
    /// Thread-safe; meant for the UI thread (ProcessDetailsPanel's selection).
    void watchProcess(std::int32_t pid);

    /// Appends to @p outSamples, oldest first, the watched process's samples from generations newer
    /// than @p lastSeenVersion that are still kept, and returns whether it appended any. Each carries
    /// the generation's own sample time, so a reader stamps history with when the data was sampled,
    /// not when it looked, and records generations published between two of its polls rather than
    /// only the latest (#1098). When nothing new has been published it returns without locking or
    /// copying anything, and a sample is a shared pointer, so polling every frame never deep-copies
    /// a snapshot (#1172). A gap between @p lastSeenVersion and the first sample's version means
    /// generations were published while more than WATCHED_SAMPLE_CAPACITY newer ones arrived
    /// unread.
    [[nodiscard]] bool watchedSamplesSince(std::uint64_t lastSeenVersion, std::vector<ProcessSample>& outSamples) const;

    /// Monotonically increasing counter, incremented each time snapshots are updated.
    /// UI can compare against a cached value to skip redundant copies when data hasn't changed.
    [[nodiscard]] std::uint64_t snapshotVersion() const;
    /// Copies the aggregated system histories into @p outHistories when their generation differs from
    /// @p lastSeenVersion (a previous ProcessSystemHistories::version) and returns whether it did. Their
    /// generation advances with every snapshot generation and when setMaxHistorySeconds() trims them,
    /// so it is not a snapshotVersion().
    [[nodiscard]] bool tryCopySystemHistoriesIfNewer(std::uint64_t lastSeenVersion, ProcessSystemHistories& outHistories) const;

    // Aggregated system-level histories derived from per-process data
    [[nodiscard]] std::vector<double> systemNetSentHistory() const;
    [[nodiscard]] std::vector<double> systemNetRecvHistory() const;
    [[nodiscard]] std::vector<double> systemPageFaultsHistory() const;
    [[nodiscard]] std::vector<double> systemThreadCountHistory() const;
    [[nodiscard]] std::vector<double> systemHandleCountHistory() const;
    [[nodiscard]] std::vector<double> systemPowerHistory() const;
    [[nodiscard]] std::vector<double> historyTimestamps() const;

    /// Sets the history window, clamped to SamplingConfig's range, and trims the system histories to
    /// it at once, advancing their generation (tryCopySystemHistoriesIfNewer()) when it has one (#1145).
    void setMaxHistorySeconds(double seconds);

    /// The per-process network rate ceiling, bytes/s ([metrics] max_sane_rate_bps, #1123). A rate
    /// above it is taken for a bad reading and shown as 0. Clamped to SamplingConfig's range.
    /// Thread-safe; takes effect from the next refresh.
    void setMaxSaneNetworkRate(double bytesPerSecond) noexcept;

    /// Number of processes in latest snapshot.
    [[nodiscard]] std::size_t processCount() const;

    /// What the underlying probe supports, as of the latest published generation. Re-read from the
    /// probe every sample: a probe can withdraw a capability after the first one (#1254). Takes the
    /// shared lock, so a per-frame reader should take them from tryCopySnapshotsIfNewer() or
    /// tryCopySystemHistoriesIfNewer() instead.
    [[nodiscard]] Platform::ProcessCapabilities capabilities() const;

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
    // The probe's capabilities as of the sample being computed: sampling thread only, under
    // m_SamplingMutex. m_PublishedCapabilities is the copy readers see, guarded by m_Mutex.
    Platform::ProcessCapabilities m_Capabilities;
    Platform::ProcessCapabilities m_PublishedCapabilities;

    // Per-process tracking state.  Consolidating previous counters and
    // peak-RSS into one struct reduces per-process map lookups
    // in computeSnapshotsLocked() from 3-4 separate finds/inserts to a single
    // try_emplace, improving cache locality and reducing map overhead.
    struct PerProcessState
    {
        Platform::ProcessCounters counters{}; // counters from last refresh (for delta)
        std::uint64_t peakRss = 0;            // tracked peak RSS
        double netSentBytesPerSec = 0.0;      // last network rates, held while the probe's read is cached
        double netReceivedBytesPerSec = 0.0;
        std::uint64_t generation = 0; // refresh generation when last seen
    };

    // Key for m_PerProcessState: the exact (pid, startTime) identity, distinct from the
    // combined hash stored in ProcessSnapshot::uniqueKey (a display/collapse-state id that
    // is not collision-free). Using the actual pair as the map key means a hash collision
    // only costs a bucket collision, not silently merging two unrelated processes' state.
    struct ProcessIdentity
    {
        std::int32_t pid = 0;
        std::uint64_t startTime = 0;

        friend bool operator==(const ProcessIdentity&, const ProcessIdentity&) = default;
    };

    struct ProcessIdentityHash
    {
        [[nodiscard]] std::size_t operator()(const ProcessIdentity& id) const noexcept
        {
            return static_cast<std::size_t>(makeUniqueKey(id.pid, id.startTime));
        }
    };

    // Single map replaces m_PrevCounters + m_PeakRss + m_ActiveKeys.
    std::unordered_map<ProcessIdentity, PerProcessState, ProcessIdentityHash> m_PerProcessState;
    // Monotonically increasing counter; bumped each computeSnapshotsLocked() call.
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
    double m_MaxHistorySeconds = Sampling::HISTORY_SECONDS_DEFAULT; // Align with Storage/System defaults

    // Latest computed snapshots. Immutable once published (replaced wholesale by the writer,
    // never mutated in place), so it's handed to readers as a shared_ptr<const ...> instead of
    // being deep-copied under the lock -- see tryCopySnapshotsIfNewer()'s doc comment.
    std::shared_ptr<const std::vector<ProcessSnapshot>> m_Snapshots = std::make_shared<const std::vector<ProcessSnapshot>>();
    std::uint64_t m_SnapshotVersion = 0;
    std::atomic<std::uint64_t> m_PublishedSnapshotVersion{0};
    // The aggregated system histories' generation (ProcessSystemHistories::version): advanced with
    // every snapshot generation and also when a history-window change trims them (#1145). Kept apart
    // from m_SnapshotVersion, whose generations each have a watched sample (watchedSamplesSince()).
    std::uint64_t m_SystemHistoryVersion = 0;
    std::atomic<std::uint64_t> m_PublishedSystemHistoryVersion{0};
    std::atomic<bool> m_InteractionActive{false};
    std::atomic<double> m_MaxSaneNetworkRateBps{Sampling::MAX_SANE_RATE_BPS_DEFAULT};
    Clock::time_point m_LastGpuMergeTime;
    bool m_HasLastGpuMergeTime = false;
    bool m_GpuMergeFailing = false; // guarded by m_SamplingMutex; logs a failing GPU merge once per streak (#1142)

    // The watched process (watchProcess()) and its latest samples, oldest first. A fixed ring --
    // m_WatchedSampleStart is the oldest slot -- so recording a sample never allocates while the
    // writer holds m_Mutex. m_WatchedPid is written under m_Mutex and also read without it by the
    // writer, which looks the process up before taking the lock.
    std::atomic<std::int32_t> m_WatchedPid{0};
    std::vector<ProcessSample> m_WatchedSamples = std::vector<ProcessSample>(WATCHED_SAMPLE_CAPACITY); // guarded by m_Mutex
    std::size_t m_WatchedSampleStart = 0;                                                              // guarded by m_Mutex
    std::size_t m_WatchedSampleCount = 0;                                                              // guarded by m_Mutex
    double m_SnapshotSampleTimeSeconds = 0.0; // guarded by m_Mutex; m_Snapshots' sample time (ProcessSample)

    // Thread safety
    mutable std::shared_mutex m_Mutex;
    std::mutex m_SamplingMutex;
    ProcessEnergy::Attributor m_EnergyAttributor; // guarded by m_SamplingMutex
    SocketTrafficAccumulator m_NetTraffic;        // guarded by m_SamplingMutex; per-process network bytes (#1099)

    // Helpers
    /// Requires m_SamplingMutex held.
    void computeSnapshotsLocked(const std::vector<Platform::ProcessCounters>& counters, std::uint64_t totalCpuTime);

    static void mergeGPUData(std::vector<ProcessSnapshot>& snapshots, const std::shared_ptr<GPUModel>& gpuModel);

    /// mergeGPUData(), contained: a throwing GPU merge must not stop process publication (#1142).
    /// On a throw the snapshots are published without GPU fields. Requires m_SamplingMutex held.
    void mergeGPUDataContained(std::vector<ProcessSnapshot>& snapshots, const std::shared_ptr<GPUModel>& gpuModel);

    /// Records @p sample as the newest watched sample, returning the one it displaced from the ring
    /// (for the caller to destroy after releasing the lock). Requires m_Mutex held exclusively.
    [[nodiscard]] ProcessSample pushWatchedSampleLocked(ProcessSample sample);

    /// A shared copy of process @p pid as @p snapshots lists it, or nullptr when it isn't listed.
    [[nodiscard]] static std::shared_ptr<const ProcessSnapshot> copyProcess(const std::vector<ProcessSnapshot>& snapshots,
                                                                            std::int32_t pid);

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
