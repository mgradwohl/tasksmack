#pragma once

#include "Domain/GPUSnapshot.h"
#include "Domain/SharedHistory.h"
#include "ISamplable.h"
#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"
#include "PublicationSlot.h"
#include "SamplingConfig.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Domain
{

/// One GPU's history as published to the UI: numeric series only, all the same length and aligned
/// sample-for-sample. Each is a view of the model's own shared series, so a publish is O(series),
/// not a copy of the history (#1412); a view stays as published whatever the model does later, and
/// converts to std::span for the charts. Whole GPUSnapshots are not kept as history at all: each
/// carries four identity strings, and with the history sized to the window (up to 18,001 samples at
/// 1800 s / 100 ms) copying them was the bulk of the sampler thread's work.
struct GPUPublishedHistory
{
    HistoryView<double> timestamps;
    HistoryView<std::uint64_t> memoryUsedBytes; ///< 0 (with memoryTotalBytes 0) where memory was unread (#1111)
    HistoryView<std::uint64_t> memoryTotalBytes;
    HistoryView<float> utilization; ///< NaN where unread or the GPU was missing (#1111, #1146); likewise below
    HistoryView<float> memoryPercent;
    HistoryView<float> gpuClock;
    HistoryView<float> encoder;
    HistoryView<float> decoder;
    HistoryView<float> temperature;
    HistoryView<float> power;
    HistoryView<float> fanSpeed;
};

struct GPUPublication
{
    std::uint64_t version = 0;
    std::vector<GPUSnapshot> snapshots;
    std::vector<Platform::GPUInfo> gpuInfo;
    /// False if enumerating the GPUs failed, in which case an empty gpuInfo means "could not look",
    /// not "looked and found none". Consumers that report the absence of a GPU must check this.
    bool gpuInfoKnown = false;
    Platform::GPUCapabilities capabilities;
    std::unordered_map<std::string, GPUPublishedHistory> histories;
};

/// What the per-process GPU breakdown needs to know about one adapter: its ids (a per-process
/// counter names its GPU by either), its name, and which memory segment its "used" figure counts.
struct GPUAdapterIdentity
{
    std::string id;
    std::string luidId; ///< Empty where the platform has no LUID
    std::string name;
    bool isIntegrated = false;
    bool memoryIsShared = false; ///< GPUInfo::memoryIsShared
};

/// Per-process GPU counters as the GPU sampler last read them (#1417). GPUModel is the single owner
/// of GPU acquisition: each refresh reads the system and the per-process counters in one probe-locked
/// section and publishes these beside GPUPublication, in a slot of their own, so the process sampler
/// merges the newest publication without ever calling the probe or waiting on its lock.
///
/// The two samplers run on their own cadences, so a process generation merges whatever was last
/// published: one publication may be merged into several process generations, and one may be
/// skipped. The per-process values stay right: the probe computes each process's utilization over
/// the interval between its own consecutive reads, which are now all made by the GPU sampler, so
/// each published figure is a rate over one GPU interval, and memory is a point reading. A consumer
/// judges how stale a publication is by captureTime (Sampling::PROCESS_GPU_DATA_MAX_AGE_MS).
struct ProcessGPUPublication
{
    std::uint64_t version = 0; ///< 0 until the first refresh has read (or tried to read) per-process data
    /// The time of the refresh that read the counters (refreshAt()'s `now`), on the steady clock.
    std::chrono::steady_clock::time_point captureTime;
    std::vector<Platform::ProcessGPUCounters> counters;
    /// The adapters as that refresh knew them (from the GPU info), to name and classify the counters' GPUs.
    std::vector<GPUAdapterIdentity> adapters;
    bool perProcessSupported = false;  ///< Read under per-process support (#1210)
    bool utilizationSupported = false; ///< ...including per-process utilization
    /// Supported, but the probe's read threw: counters is empty, a gap rather than idle processes.
    bool readFailed = false;
};

struct TransparentStringHash
{
    // NOLINTNEXTLINE(readability-identifier-naming) - STL transparent hashing requires this exact alias name.
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept
    {
        return std::hash<std::string_view>{}(value);
    }
};

struct TransparentStringEqual
{
    // NOLINTNEXTLINE(readability-identifier-naming) - STL transparent equality requires this exact alias name.
    using is_transparent = void;

    [[nodiscard]] bool operator()(std::string_view lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }
};

/// Current snapshots keyed by GPU id. Unordered: anything shown to the user goes through
/// orderSnapshotsByEnumeration() first (#1163).
using GPUSnapshotMap = std::unordered_map<std::string, GPUSnapshot, TransparentStringHash, TransparentStringEqual>;

/// The snapshots in a stable order: the enumeration order of gpuInfo first, then any GPU the read
/// returned that enumeration did not list, sorted by id. Iterating the map directly gave a hash order,
/// so a GPU dropping out of one read could reorder the rest and shift their UI state (#1163).
[[nodiscard]] std::vector<GPUSnapshot> orderSnapshotsByEnumeration(std::span<const Platform::GPUInfo> gpuInfo,
                                                                   const GPUSnapshotMap& snapshots);

/// Thread-safe: the sampler thread refreshes while the UI thread reads. Writers (refreshAt() and
/// setMaxHistorySeconds()) are serialised on m_WriterMutex; each builds its publication outside every
/// lock a reader takes and swaps it in through m_Publication, so publication() never waits for a
/// history copy (#868).
class GPUModel : public ISamplable
{
  public:
    explicit GPUModel(std::unique_ptr<Platform::IGPUProbe> probe);
    ~GPUModel() override = default;

    GPUModel(const GPUModel&) = delete;
    GPUModel& operator=(const GPUModel&) = delete;
    GPUModel(GPUModel&&) = delete;
    GPUModel& operator=(GPUModel&&) = delete;

    /// Perform one sampling iteration (ISamplable implementation).
    void sample() override
    {
        refresh();
    }

    // Refresh metrics (called by sampler thread)
    void refresh();

    /// Same as refresh(), but with an explicit "now" instead of reading
    /// std::chrono::steady_clock::now(), so time-based history trimming can be tested
    /// deterministically. Mirrors StorageModel::sampleAt().
    void refreshAt(std::chrono::steady_clock::time_point now);

    /// History window in seconds. Like SystemModel and StorageModel, samples older than this
    /// are dropped, so the GPU charts cover the same window as every other chart (#993). Clamped to
    /// SamplingConfig's range; trims and republishes at once, once anything has been published (#1145).
    void setMaxHistorySeconds(double seconds);
    [[nodiscard]] double maxHistorySeconds() const;

    // Get current snapshots (thread-safe)
    [[nodiscard]] std::vector<GPUSnapshot> snapshots() const;

    // Get flattened history arrays for specific GPU (for chart plotting)
    [[nodiscard]] std::vector<float> utilizationHistory(std::string_view gpuId) const;
    [[nodiscard]] std::vector<float> memoryPercentHistory(std::string_view gpuId) const;
    [[nodiscard]] std::vector<float> gpuClockHistory(std::string_view gpuId) const;
    [[nodiscard]] std::vector<float> encoderHistory(std::string_view gpuId) const;
    [[nodiscard]] std::vector<float> decoderHistory(std::string_view gpuId) const;
    [[nodiscard]] std::vector<float> temperatureHistory(std::string_view gpuId) const;
    [[nodiscard]] std::vector<float> powerHistory(std::string_view gpuId) const;
    [[nodiscard]] std::vector<float> fanSpeedHistory(std::string_view gpuId) const;

    // Get global timestamps for all GPU history samples (one per refresh call)
    [[nodiscard]] std::vector<double> historyTimestamps() const;

    // Get per-GPU timestamps: one per refresh since the GPU was first seen, including refreshes it
    // was missing from, whose history entries are NaN gaps (#1146). Length matches the per-GPU
    // history vectors (utilizationHistory, etc.).
    [[nodiscard]] std::vector<double> historyTimestamps(std::string_view gpuId) const;

    // GPU info: enumerated at construction, and again whenever the probe's rescanGPUs() reports a
    // change (a GPU added, removed or lost, or a sleeping adapter's sensors now discoverable) (#1116,
    // #1289). Each refresh's publication carries the current list.
    [[nodiscard]] std::vector<Platform::GPUInfo> gpuInfo() const;

    // Capabilities (re-read along with the GPU info)
    [[nodiscard]] Platform::GPUCapabilities capabilities() const;

    /// True once the probe's capabilities are known and say it has no per-process GPU metrics (DRM-
    /// or ROCm-only systems, DXGI alone): per-process GPU usage cannot be observed here. False while
    /// they are unknown. A single atomic load, for per-frame readers (#1210).
    [[nodiscard]] bool perProcessMetricsKnownUnsupported() const noexcept
    {
        return m_PerProcessKnownUnsupported.load(std::memory_order_acquire);
    }

    /// True once the probe's capabilities are known and say it reports no per-process utilization,
    /// though it may report per-process memory and engines (NVML's running-process lists): GPU %
    /// would read 0 for every process (#1210). False while they are unknown. A single atomic load.
    [[nodiscard]] bool perProcessUtilizationKnownUnsupported() const noexcept
    {
        return m_PerProcessUtilizationKnownUnsupported.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::shared_ptr<const GPUPublication> publication() const noexcept;
    [[nodiscard]] std::uint64_t publicationVersion() const noexcept;

    /// The newest per-process GPU publication (#1417): a shared_ptr copy, never a probe call, so the
    /// process sampler can take it on every refresh. Version 0 (empty) until the first refresh().
    [[nodiscard]] std::shared_ptr<const ProcessGPUPublication> processGPUPublication() const noexcept
    {
        return m_ProcessPublication.load();
    }
    [[nodiscard]] std::uint64_t processGPUPublicationVersion() const noexcept
    {
        return m_ProcessPublication.version();
    }

    /// Read the per-process GPU counters from the probe now, under the probe lock: a direct read for
    /// tests and benchmarks. The process sampler merges processGPUPublication() instead, which
    /// refresh() fills, so it never runs the probe on its own thread (#1417).
    [[nodiscard]] std::vector<Platform::ProcessGPUCounters> readProcessGPUCounters() const;

    /// Per-process GPU counters together with the per-process support they were read under (#1210).
    struct ProcessGPUReading
    {
        std::vector<Platform::ProcessGPUCounters> counters;
        bool perProcessSupported = false;  ///< Not known to lack per-process metrics
        bool utilizationSupported = false; ///< ...nor per-process utilization among them
        /// What the probe's read threw, if it did. It is carried here rather than thrown, so the
        /// caller still learns the support the failed read ran under (#1210); rethrow it with
        /// std::rethrow_exception() after taking the flags.
        std::exception_ptr failure;
    };

    /// readProcessGPUCounters() and the support it was read under, from one operation: the flags are
    /// read under the probe lock, which a re-enumeration that changes them holds, so the counters
    /// and the flags always agree. Reading the flags separately could stamp a generation supported
    /// while the read short-circuited empty, or the reverse (#1210).
    [[nodiscard]] ProcessGPUReading readProcessGPUData() const;

  private:
    std::unique_ptr<Platform::IGPUProbe> m_Probe;
    mutable std::mutex m_ProbeMutex;
    // The per-process GPU publication (#1417). Committed by refreshAt() while it holds m_ProbeMutex,
    // which therefore also guards the version counter and the failure streak flag: generations are
    // committed in read order even if two refreshes race. The slot's own lock is a leaf.
    PublicationSlot<ProcessGPUPublication> m_ProcessPublication;
    std::uint64_t m_ProcessPublicationVersion = 0; // guarded by m_ProbeMutex
    bool m_ProcessReadFailing = false;             // guarded by m_ProbeMutex; logs a failing read once per streak
    // The GPU info, its known flag and the capabilities are written only on the sampler thread
    // (the constructor, then rescanGPUs() inside refreshAt()), always under a unique m_Mutex, so the
    // sampler thread may read them without a lock; any other thread takes m_Mutex shared.
    std::vector<Platform::GPUInfo> m_GPUInfo;
    // False while no enumerateGPUs() has succeeded, leaving m_GPUInfo empty for a reason other
    // than there being no GPUs. Published as GPUPublication::gpuInfoKnown.
    bool m_GPUInfoKnown = false;
    Platform::GPUCapabilities m_Capabilities;
    // False while no capabilities() query has succeeded, leaving m_Capabilities at its
    // default (all-false) values. readProcessGPUCounters() must not treat that as proof
    // per-process metrics are unsupported -- see its use of this flag for why.
    bool m_CapabilitiesKnown = false;
    // m_CapabilitiesKnown && !m_Capabilities.hasPerProcessMetrics, kept in step with both
    // (written with them, under the unique m_Mutex). The per-process read (every refresh, and
    // readProcessGPUCounters()) returns straight away on a backend that has no per-process data:
    // reading this flag instead of taking m_Mutex shared for the two fields keeps that early exit to
    // a single atomic load (#1322). ProcessModel reads it too, before the first publication (#1417).
    std::atomic<bool> m_PerProcessKnownUnsupported{false};
    // m_CapabilitiesKnown && !m_Capabilities.hasPerProcessUtilization, kept in step the same way (#1210)
    std::atomic<bool> m_PerProcessUtilizationKnownUnsupported{false};

    /// One sample of a GPU's history, as its series record it: a field that was not read, or a
    /// placeholder for a refresh the GPU was missing from, is NaN (#1111, #1146).
    struct HistorySample
    {
        double timestamp = 0.0;
        std::uint64_t memoryUsedBytes = 0; // 0 with memoryTotalBytes 0: no byte figures (#1111)
        std::uint64_t memoryTotalBytes = 0;
        float utilization = 0.0F;
        float memoryPercent = 0.0F;
        float gpuClock = 0.0F;
        float encoder = 0.0F;
        float decoder = 0.0F;
        float temperature = 0.0F;
        float power = 0.0F;
        float fanSpeed = 0.0F;
        bool sampled = true; // false for a placeholder
    };
    /// What @p sample's history records: the same values the per-field accessors return.
    [[nodiscard]] static HistorySample historySample(const GPUSnapshot& sample) noexcept;
    /// The placeholder recorded at @p nowSeconds for a GPU missing from a read (#1146): every reading NaN.
    [[nodiscard]] static HistorySample placeholderSample(double nowSeconds) noexcept;

    /// One GPU's history: a shared series per published field, all the same length and aligned
    /// sample-for-sample, so publishing it is a view of each (#1412). Appends are staged: reserve()
    /// makes every allocation, and the push() that follows cannot fail part way.
    struct GPUSeries
    {
        SharedHistoryBuffer<double> timestamps;
        SharedHistoryBuffer<std::uint64_t> memoryUsedBytes;
        SharedHistoryBuffer<std::uint64_t> memoryTotalBytes;
        SharedHistoryBuffer<float> utilization;
        SharedHistoryBuffer<float> memoryPercent;
        SharedHistoryBuffer<float> gpuClock;
        SharedHistoryBuffer<float> encoder;
        SharedHistoryBuffer<float> decoder;
        SharedHistoryBuffer<float> temperature;
        SharedHistoryBuffer<float> power;
        SharedHistoryBuffer<float> fanSpeed;
        // Placeholders at the newest end. The window holds a real sample while this is below size().
        std::size_t trailingPlaceholders = 0;

        explicit GPUSeries(std::size_t capacity) noexcept;
        void setCapacity(std::size_t capacity) noexcept;
        /// Room for @p count more samples in every series. Throws std::bad_alloc with nothing observable changed.
        void reserve(std::size_t count);
        /// Append one sample to every series. Doesn't allocate after reserve(1), so doesn't throw then.
        void push(const HistorySample& sample);
        void discardFront(std::size_t count) noexcept;
        [[nodiscard]] std::size_t size() const noexcept
        {
            return timestamps.size();
        }
        [[nodiscard]] bool hasReading() const noexcept
        {
            return trailingPlaceholders < size();
        }
        [[nodiscard]] GPUPublishedHistory view() const noexcept;
    };

    // Current snapshots per GPU
    using SnapshotMap = GPUSnapshotMap;
    using HistoryMap = std::unordered_map<std::string, GPUSeries, TransparentStringHash, TransparentStringEqual>;
    using CounterMap = std::unordered_map<std::string, Platform::GPUCounters, TransparentStringHash, TransparentStringEqual>;

    SnapshotMap m_Snapshots;

    // History per GPU, from its first sample: a refresh it was missing from has a placeholder (#1146).
    HistoryMap m_Histories;

    // One timestamp per refresh (the per-GPU series carry their own).
    SharedHistoryBuffer<double> m_HistoryTimestamps{Sampling::historyCapacityForSeconds(Sampling::HISTORY_SECONDS_DEFAULT)};

    // History window; ring capacities are sized from it by applyHistoryCapacity().
    double m_MaxHistorySeconds = Sampling::HISTORY_SECONDS_DEFAULT;

    // Previous counters for rate calculation
    CounterMap m_PrevCounters;
    std::chrono::steady_clock::time_point m_PrevSampleTime;
    // When the probe last got a GPURescan::Full (construction counts as one).
    std::chrono::steady_clock::time_point m_LastFullRescan;

    // Thread safety. Lock order: m_WriterMutex -> m_Mutex -> the publication slot's mutex, and
    // m_ProbeMutex -> m_Mutex, and m_ProbeMutex -> m_ProcessPublication's mutex (a leaf); m_ProbeMutex and m_WriterMutex are never held
    // together, so a slow probe read never holds up setMaxHistorySeconds() on the UI thread.
    //
    // m_WriterMutex serialises the writers, refreshAt() (from computing the snapshots) and
    // setMaxHistorySeconds(), through the publication commit, so generations are numbered and
    // committed in order. Readers never take it, and it is never taken while holding m_Mutex.
    // m_PrevCounters and m_PrevSampleTime are writer-only state under it.
    std::mutex m_WriterMutex;
    // Guards the state above for the per-field accessors: writers mutate it exclusively, and
    // publish() reads it under a shared lock, so neither publication() nor those accessors wait on
    // a publication's copy.
    mutable std::shared_mutex m_Mutex;
    PublicationSlot<GPUPublication> m_Publication;
    std::uint64_t m_PublicationVersion = 0; // guarded by m_WriterMutex; the last committed generation

    // Helper: compute snapshot from current/previous counters
    [[nodiscard]] GPUSnapshot
    computeSnapshot(const Platform::GPUCounters& current, const Platform::GPUCounters* previous, double timeDeltaSeconds) const;

    /// A copy of one series of @p gpuId's history; empty for an unknown GPU.
    template<typename T> [[nodiscard]] std::vector<T> copySeries(std::string_view gpuId, SharedHistoryBuffer<T> GPUSeries::* series) const;
    /// Build the next generation from the history state under a shared lock, then commit it.
    /// Requires m_WriterMutex held and m_Mutex not held.
    void publish();

    /// readProcessGPUData()'s read, for a caller that already holds m_ProbeMutex. Never throws the
    /// probe's exception: it is carried in the result.
    [[nodiscard]] ProcessGPUReading readProcessGPUDataLocked() const;

    /// Read the per-process counters and commit them as the next ProcessGPUPublication, stamped
    /// @p now (#1417). Caller holds m_ProbeMutex. A failed probe read is published as readFailed; a
    /// failure to build the publication (bad_alloc) is logged and leaves the previous one in place.
    void publishProcessGPUData(std::chrono::steady_clock::time_point now);

    // Ask the probe whether the GPU set or its GPUInfo changed (a full rescan every
    // GPU_RESCAN_INTERVAL_SECONDS, a quick one otherwise) and, if so, re-enumerate and take the new
    // GPU info and capabilities (#1116, #1289). Also retries a failed startup enumeration or
    // capabilities query at the full-rescan rate. Caller holds m_ProbeMutex, not m_Mutex.
    void rescanGPUs(std::chrono::steady_clock::time_point now);

    /// One refresh's history append, staged: every allocation it needs is made here, so applying it
    /// cannot fail part way (#1412).
    struct PendingHistory
    {
        HistoryMap newGpus; // GPUs this read reports that have no history yet, with room for the sample
    };
    /// Create the series of the GPUs @p snapshots introduces in @p pending, and reserve room for one
    /// more sample in every series and the hash buckets the new GPUs will use. May throw; changes
    /// nothing observable. Caller holds m_Mutex exclusively.
    void stageHistoryAppend(PendingHistory& pending, const SnapshotMap& snapshots);
    /// Apply a staged append: adopt the new GPUs' series, then append @p snapshots' samples, and a
    /// placeholder for every GPU with a history that is missing from them (#1146). Uses only what
    /// stageHistoryAppend() reserved, so it does not allocate or throw. Caller holds m_Mutex exclusively.
    void commitHistoryAppend(PendingHistory& pending, const SnapshotMap& snapshots, double nowSeconds) noexcept;

    // Size every history series for m_MaxHistorySeconds at the fastest refresh cadence (caller holds m_Mutex).
    void applyHistoryCapacity() noexcept;
    // Drop samples older than m_MaxHistorySeconds before nowSeconds, except the newest of them while a
    // newer sample remains, like HistoryUtils::discardBefore (#1016), and forget a GPU whose window
    // holds only placeholders (caller holds m_Mutex).
    void trimHistory(double nowSeconds) noexcept;
};

} // namespace Domain
