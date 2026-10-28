#pragma once

#include "DeferredProbe.h"
#include "History.h"
#include "ISamplable.h"
#include "PidIndex.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ProcessTypes.h"
#include "ProcessEnergyAttribution.h"
#include "ProcessRowHistory.h"
#include "ProcessSnapshot.h"
#include "SamplingConfig.h"
#include "SharedHistory.h"
#include "SingleLineText.h"
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
#include <string>
#include <unordered_map>
#include <vector>

namespace Domain
{

// Forward declarations
class GPUModel;
class ServiceModel;
struct ProcessGPUPublication;

/// One generation of the aggregated system histories, aligned by timestamps. Each series is a view of
/// the model's shared, append-only storage (#1412, #2156): taking a generation copies five shared_ptrs,
/// not the samples, and a view stays valid and unchanged for as long as the reader holds it.
struct ProcessSystemHistories
{
    std::uint64_t version = 0;
    HistoryView<double> timestamps;
    HistoryView<double> power;
    HistoryView<double> pageFaults;
    HistoryView<double> threadCount;
    HistoryView<double> handleCount;
    // The probe's capabilities as of the latest published generation (#1254): a probe can withdraw
    // one after the first sample (Windows' EStats check, #1161), so they aren't fixed at startup.
    Platform::ProcessCapabilities capabilities;
};

/// One sample of the aggregated system histories (ProcessSystemHistories), for
/// ProcessModel::appendSystemHistory().
struct ProcessSystemHistorySample
{
    double timeSeconds = 0.0; ///< steady_clock seconds since its epoch, as the histories' timestamps
    double pageFaultsPerSec = 0.0;
    double threadCount = 0.0;
    double handleCount = 0.0;
    double powerWatts = 0.0;
};

/// Owns a process probe, caches previous counters, and computes CPU% deltas.
/// Call refresh() periodically; snapshots() returns the latest computed data.
/// Thread-safe: can receive updates from background sampler.
class ProcessModel : public ISamplable
{
  public:
    using Clock = std::chrono::steady_clock;
    using NowFunction = std::function<Clock::time_point()>;

    /// What the GPU probe supplied per process for a snapshot generation (#1210): the support its
    /// per-process GPU counters were read under, published with the generation.
    struct GpuSupport
    {
        bool perProcess = false;     ///< Per-process GPU data at all
        bool utilization = false;    ///< Per-process utilization among it
        bool npuPerProcess = false;  ///< Per-process NPU / compute-accelerator data at all (#1827)
        bool npuUtilization = false; ///< Per-process NPU utilization among it
        /// The probe supports per-process GPU or NPU data, but reading it failed for this generation:
        /// its GPU and NPU fields are a gap, not a measurement -- and not "not available on this system".
        bool readFailed = false;

        friend bool operator==(const GpuSupport&, const GpuSupport&) = default;
    };

    explicit ProcessModel(std::unique_ptr<Platform::IProcessProbe> probe, NowFunction now = [] { return Clock::now(); });
    /// A model whose probe arrives later, through setProbe() (#2731): nothing is published and the
    /// capabilities are the defaults until it does.
    explicit ProcessModel(DeferredProbeTag /*tag*/, NowFunction now = [] { return Clock::now(); });
    ~ProcessModel() override = default;

    /// Gives a model built with DeferredProbeTag its probe (#2731). Reads the probe's capabilities, ticks
    /// per second and total memory, as the constructor did; the probe is not otherwise read. Meant for
    /// the sampler thread, before its first sample (the sampler's start hook); thread-safe, and a probe
    /// already held is replaced.
    void setProbe(std::unique_ptr<Platform::IProcessProbe> probe);

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

    /// Test and benchmark seam (#2155): the app samples through refresh(). Applies externally
    /// provided counters as one sample. Thread-safe.
    void updateFromCounters(const std::vector<Platform::ProcessCounters>& counters, std::uint64_t totalCpuTime);

    /// Get latest computed snapshots (copy for thread safety).
    [[nodiscard]] std::vector<ProcessSnapshot> snapshots() const;

    /// Find a single snapshot by PID without copying the full snapshot vector.
    /// Always reflects the latest published data, unlike a version-gated render cache.
    [[nodiscard]] std::optional<ProcessSnapshot> findSnapshot(std::int32_t pid) const;

    /// Copy the snapshot generation only when a newer version exists. "Copy" is a shared_ptr
    /// assignment (an O(1) refcount bump), not a deep copy of the underlying vector: the
    /// vector is immutable once published, so readers can share it directly instead of each
    /// duplicating every process's data under the lock. Returns true and updates outSnapshots
    /// when a newer generation is available; otherwise returns false and leaves outSnapshots
    /// untouched. With @p outCapabilities, the probe's capabilities published with that generation
    /// are copied into it under the same lock (#1254), so a reader keeps them current at no extra cost.
    /// With @p outGpuSupport, likewise the GPU support that generation's GPU fields were read under
    /// (#1210): not the GPU model's current state, which can have moved on since its last
    /// per-process publication (#1417). With @p outRowHistory, likewise the per-process histories
    /// published with that generation (#1612), row-aligned with it; nullptr while none are kept.
    [[nodiscard]] bool tryCopySnapshotsIfNewer(std::uint64_t lastSeenVersion,
                                               std::shared_ptr<const std::vector<ProcessSnapshot>>& outSnapshots,
                                               std::uint64_t& outVersion,
                                               Platform::ProcessCapabilities* outCapabilities = nullptr,
                                               GpuSupport* outGpuSupport = nullptr,
                                               std::shared_ptr<const ProcessRowHistory::Generation>* outRowHistory = nullptr) const;

    /// Which per-process histories to keep for the Processes table's sparklines (#1612): from the next
    /// generation on, each publishes a ring per process per metric in @p metrics, row-aligned with its
    /// snapshots (tryCopySnapshotsIfNewer()'s @p outRowHistory). An empty set -- the default -- keeps
    /// none: the next generation drops every ring and publishes none. Thread-safe and lock-free.
    void setRowHistoryMetrics(ProcessRowHistory::MetricSet metrics) noexcept;

    /// Test seam (#2155): what setRowHistoryMetrics() last set, for tests of the panels that set it.
    [[nodiscard]] ProcessRowHistory::MetricSet rowHistoryMetrics() const noexcept;

    /// How many of the watched process's samples are kept for watchedSamplesSince(): every generation
    /// for 6.4 s at the fastest refresh interval, so a reader that polls once per UI frame -- or once
    /// per wake, at most 1 s apart, while the hidden window draws no frames (#3156) -- misses none
    /// unless the UI stalls for longer than that.
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
    /// so it is not a snapshotVersion(). The series are shared views, not copies (#2156): this is
    /// O(series) under the shared lock, whatever the history length, and consecutive generations'
    /// views share their storage.
    [[nodiscard]] bool tryCopySystemHistoriesIfNewer(std::uint64_t lastSeenVersion, ProcessSystemHistories& outHistories) const;

    /// Sets the history window, clamped to SamplingConfig's range, and trims the system histories to
    /// it at once, advancing their generation (tryCopySystemHistoriesIfNewer()) when it has one (#1145).
    /// Changing the histories' capacity never allocates (#2156), so the five series always keep the
    /// same capacity and length.
    void setMaxHistorySeconds(double seconds);

    /// Fills in the next sample of a series, returning false when there are no more.
    using SystemHistorySource = std::function<bool(ProcessSystemHistorySample& sample)>;

    /// Appends samples to the aggregated system histories, oldest first, trims them to the window
    /// and advances their generation once: a history preload (the synthetic scenario fills the whole
    /// window at startup, #1413). These histories are sums over every process, so preloading them by
    /// running thousands of processes through refresh() 18k times would take minutes. A sample not
    /// later than the newest one held is skipped, so the histories stay in time order. @p next runs
    /// under the model's lock and must not call back into it. Thread-safe.
    void appendSystemHistory(const SystemHistorySource& next);

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

    /// Set GPU model for per-process GPU data. When set, refresh() merges the per-process GPU
    /// counters the GPU model's own sampler last published (GPUModel::processGPUPublication()); it
    /// never calls the GPU probe, so a slow GPU driver read doesn't hold up the process list (#1417).
    void setGPUModel(std::shared_ptr<GPUModel> gpuModel);

    /// Set the service list that says which services each process hosts (#1880). When set, refresh()
    /// fills each snapshot's services from the list the service model last published, if it is current
    /// (ProcessServices::isCurrent()); it never samples the service model itself, whose own sampler
    /// keeps it current while something needs it. Its PIDs also make ProcessCapabilities::hasServices
    /// true. The Linux unit from the probe is merged whether or not one is set.
    void setServiceModel(std::shared_ptr<const ServiceModel> serviceModel);

  private:
    // Set by the constructor or setProbe() (under m_SamplingMutex); refresh()'s null check reads it outside
    // the lock, on the one sampling thread (the sampler's, which also runs setProbe() from its start
    // hook), as before.
    std::unique_ptr<Platform::IProcessProbe> m_Probe;
    NowFunction m_Now;
    std::shared_ptr<GPUModel> m_GPUModel;               // For per-process GPU data
    std::shared_ptr<const ServiceModel> m_ServiceModel; // For the services each process hosts (#1880), guarded by m_Mutex
    // The probe's capabilities as of the sample being computed: sampling thread only, under
    // m_SamplingMutex. m_PublishedCapabilities is the copy readers see, guarded by m_Mutex.
    Platform::ProcessCapabilities m_Capabilities;
    Platform::ProcessCapabilities m_PublishedCapabilities;
    // What the GPU probe supplied per process for the published generation (ProcessSample, #1210),
    // guarded by m_Mutex like m_PublishedCapabilities.
    GpuSupport m_PublishedGpuSupport;

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
        Clock::time_point firstSeen;  // sample time of the generation that first listed it (#1417)
        // Single-line forms of counters.name / counters.command, re-derived only when those change (#1624).
        SingleLineMemo name;
        SingleLineMemo command;
    };

    // Key for m_PerProcessState: the exact (pid, startTime) identity, distinct from the
    // combined hash stored in ProcessSnapshot::uniqueKey (a display/collapse-state id that
    // is unlikely to collide since #2115, but not collision-free). Using the actual pair as the map key means a hash collision
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
    // Set while computeSnapshotsLocked() walks the processes. Still set at the next call means the
    // last one threw part way, possibly between a process's SingleLineMemo update and its counters
    // being stored, so that call re-derives every single-line form rather than trust them (#1624).
    bool m_SnapshotBuildInterrupted = false;
    // The process tree's PID -> row table, rebuilt by every computeSnapshotsLocked() (#2981). Writer
    // scratch, guarded by m_SamplingMutex: kept between refreshes only for its storage, never read
    // outside the tree build and never part of a published snapshot.
    PidIndex m_PidIndex;
    // The processes carried over (ProcessCounters::heldOver) since their counters were read (#3031), with the
    // system CPU total and sample time of that read: their next real read's rates are taken over the whole
    // time since. Empty nearly always; kept apart from PerProcessState so every other process pays nothing.
    struct HeldOverSince
    {
        std::uint64_t totalCpuTime = 0;
        Clock::time_point sampleTime;
    };
    std::unordered_map<ProcessIdentity, HeldOverSince, ProcessIdentityHash> m_HeldOver;

    std::uint64_t m_PrevTotalCpuTime = 0;
    std::uint64_t m_SystemTotalMemory = 0; // For memoryPercent calculation
    long m_TicksPerSecond = 100;           // For cpuTimeSeconds calculation
    Clock::time_point m_PrevSampleTime;    // For rate calculations (network, I/O, power)
    bool m_HasPrevSampleTime = false;

    // Aggregated system histories (aligned by timestamps), guarded by m_Mutex. Capacity is set from
    // the configured window; time-based trimming via discardFront keeps them in the window. Shared,
    // append-only storage (#2156), so tryCopySystemHistoriesIfNewer() hands out views rather than
    // copying every sample. An append reserves room in all five before pushing to any
    // (appendSystemHistoryLocked()), so a std::bad_alloc leaves them aligned.
    SharedHistoryBuffer<double> m_SystemPageFaultsHistory;
    SharedHistoryBuffer<double> m_SystemThreadCountHistory;
    SharedHistoryBuffer<double> m_SystemHandleCountHistory;
    SharedHistoryBuffer<double> m_SystemPowerHistory;
    SharedHistoryBuffer<double> m_Timestamps;
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
    std::atomic<double> m_MaxSaneNetworkRateBps{Sampling::MAX_SANE_RATE_BPS_DEFAULT};
    bool m_GpuMergeFailing = false;            // guarded by m_SamplingMutex; logs a failing GPU merge once per streak (#1142)
    bool m_GpuDataStale = false;               // guarded by m_SamplingMutex; logs stale per-process GPU data once per streak (#1417)
    bool m_SystemHistoryAppendFailing = false; // guarded by m_SamplingMutex; logs a failing history append once per streak (#2156)

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

    // Per-process histories for the sparklines (#1612): which metrics to keep (set from the UI
    // thread), the rings carried between generations (sampling thread only), and the generation's
    // published rings (guarded by m_Mutex, replaced with m_Snapshots so the two always match).
    std::atomic<ProcessRowHistory::MetricSet> m_RowHistoryMetrics{0};
    ProcessRowHistory::Tracker m_RowHistoryTracker; // guarded by m_SamplingMutex
    std::shared_ptr<const ProcessRowHistory::Generation> m_RowHistory;

    // Helpers
    /// Requires m_SamplingMutex held.
    void computeSnapshotsLocked(const std::vector<Platform::ProcessCounters>& counters, std::uint64_t totalCpuTime);

    /// For a carried-over process (ProcessCounters::heldOver, #3031), appends its last row to @p newSnapshots,
    /// taking the last generation into @p previousSnapshots the first time, and notes when its counters were
    /// last read (@p lastSampleTime, m_PrevTotalCpuTime) unless already noted. False when it has no last row.
    /// Out of line, never inlined: computeSnapshotsLocked()'s loop runs for every process, and this for
    /// nearly none. Requires m_SamplingMutex held.
    [[gnu::noinline]] [[nodiscard]] bool appendHeldOverLocked(const Platform::ProcessCounters& current,
                                                              const ProcessIdentity& key,
                                                              Clock::time_point lastSampleTime,
                                                              std::shared_ptr<const std::vector<ProcessSnapshot>>& previousSnapshots,
                                                              std::vector<ProcessSnapshot>& newSnapshots);

    /// The interval a process's rates are taken over (#3031), as the system CPU ticks, seconds and
    /// microseconds since its counters were last read.
    struct RateInterval
    {
        std::uint64_t totalCpuDelta = 0;
        double elapsedSeconds = 0.0;
        std::uint64_t timeDeltaUs = 0;
    };

    /// @p interval, or, for a process carried over since its counters were last read, the interval since
    /// that read, forgetting it (#3031). Never inlined, as appendHeldOverLocked(). Requires m_SamplingMutex held.
    [[gnu::noinline]] [[nodiscard]] RateInterval
    rateIntervalAfterHeldOverLocked(const ProcessIdentity& key, RateInterval interval, std::uint64_t totalCpuTime, Clock::time_point now);

    /// Merges @p publication's per-process GPU counters into @p snapshots. @p firstSeen holds, for
    /// each snapshot, when this model first listed that process: a process first listed after the
    /// publication was read has GPU fields that were never read (#1210, #1417): an entry for its
    /// pid belongs to an exited process that held the pid at the read, and is not merged.
    static void mergeGPUData(std::vector<ProcessSnapshot>& snapshots,
                             const std::vector<Clock::time_point>& firstSeen,
                             const ProcessGPUPublication& publication);

    /// The per-process GPU fields of the generation sampled at @p now, from @p gpuModel's newest
    /// publication (#1417) -- never a probe call -- and the support they were read under (#1210).
    /// Nothing published yet: no fields read. Older than Sampling::PROCESS_GPU_DATA_MAX_AGE_MS, or a
    /// failed read: published without GPU fields and marked readFailed, so the cells read as
    /// unreadable rather than as measured zeros or as unsupported. Contained: a throwing merge
    /// (bad_alloc) must not stop process publication (#1142). Requires m_SamplingMutex held.
    GpuSupport mergeGPUDataContained(std::vector<ProcessSnapshot>& snapshots,
                                     const std::vector<Clock::time_point>& firstSeen,
                                     const GPUModel& gpuModel,
                                     Clock::time_point now);

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
                                                         std::uint64_t timeDeltaUs,
                                                         unsigned pageFaultCountBits);

    void trimHistory();
    void applyHistoryCapacity() noexcept;
    /// Appends @p sample to all five system histories, or on std::bad_alloc to none of them: room is
    /// reserved in every series before any is pushed to. Requires m_Mutex held exclusively.
    void appendSystemHistoryLocked(const ProcessSystemHistorySample& sample);

    [[nodiscard]] static std::uint64_t makeUniqueKey(std::int32_t pid, std::uint64_t startTime);
    [[nodiscard]] static std::string translateState(char rawState);
};

} // namespace Domain
