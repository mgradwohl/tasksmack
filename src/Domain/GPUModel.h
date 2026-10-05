#pragma once

#include "Domain/GPUSnapshot.h"
#include "Domain/History.h"
#include "ISamplable.h"
#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"
#include "SamplingConfig.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Domain
{

/// One GPU's history as published to the UI: numeric series only, all the same length and aligned
/// sample-for-sample. Whole GPUSnapshots are not republished: each carries four identity strings,
/// and with the history sized to the window (up to 18,001 samples at 1800 s / 100 ms) copying them
/// on every refresh was the bulk of the sampler thread's work. GPUModel::history() still returns
/// them for a caller that needs one.
struct GPUPublishedHistory
{
    std::vector<double> timestamps;
    std::vector<std::uint64_t> memoryUsedBytes;
    std::vector<std::uint64_t> memoryTotalBytes;
    std::vector<float> utilization;
    std::vector<float> memoryPercent;
    std::vector<float> gpuClock;
    std::vector<float> encoder;
    std::vector<float> decoder;
    std::vector<float> temperature;
    std::vector<float> power;
    std::vector<float> fanSpeed;
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
    /// are dropped, so the GPU charts cover the same window as every other chart (#993).
    void setMaxHistorySeconds(double seconds);
    [[nodiscard]] double maxHistorySeconds() const;

    // Get current snapshots (thread-safe)
    [[nodiscard]] std::vector<GPUSnapshot> snapshots() const;

    // Get history for specific GPU (returns copy for thread safety)
    [[nodiscard]] std::vector<GPUSnapshot> history(std::string_view gpuId) const;

    // Get a single historical snapshot by logical index (0 = oldest).
    // Returns nullopt if gpuId is unknown or index is out of range.
    // Prefer this over history() when only one sample is needed (avoids copying the full vector).
    [[nodiscard]] std::optional<GPUSnapshot> snapshotAt(std::string_view gpuId, std::size_t index) const;

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

    // GPU info (static, rarely changes)
    [[nodiscard]] std::vector<Platform::GPUInfo> gpuInfo() const;

    // Capabilities
    [[nodiscard]] Platform::GPUCapabilities capabilities() const;
    [[nodiscard]] std::shared_ptr<const GPUPublication> publication() const noexcept;
    [[nodiscard]] std::uint64_t publicationVersion() const noexcept;

    // Per-process GPU counters (called by ProcessModel to enrich process snapshots)
    [[nodiscard]] std::vector<Platform::ProcessGPUCounters> readProcessGPUCounters() const;

  private:
    std::unique_ptr<Platform::IGPUProbe> m_Probe;
    mutable std::mutex m_ProbeMutex;
    std::vector<Platform::GPUInfo> m_GPUInfo;
    // False if the constructor's enumerateGPUs() threw, leaving m_GPUInfo empty for a reason other
    // than there being no GPUs. Published as GPUPublication::gpuInfoKnown.
    bool m_GPUInfoKnown = false;
    Platform::GPUCapabilities m_Capabilities;
    // False if the constructor's capabilities() query threw, leaving m_Capabilities at its
    // default (all-false) values. readProcessGPUCounters() must not treat that as proof
    // per-process metrics are unsupported -- see its use of this flag for why.
    bool m_CapabilitiesKnown = false;

    // Current snapshots per GPU
    using SnapshotMap = GPUSnapshotMap;
    using HistoryMap = std::unordered_map<std::string, HistoryBuffer<GPUSnapshot>, TransparentStringHash, TransparentStringEqual>;
    using CounterMap = std::unordered_map<std::string, Platform::GPUCounters, TransparentStringHash, TransparentStringEqual>;

    SnapshotMap m_Snapshots;

    // History buffers per GPU
    HistoryMap m_Histories;

    // Timestamps for history data
    std::vector<double> m_HistoryTimestamps;

    // History window; ring capacities are sized from it by applyHistoryCapacity().
    double m_MaxHistorySeconds = Sampling::HISTORY_SECONDS_DEFAULT;

    // Previous counters for rate calculation
    CounterMap m_PrevCounters;
    std::chrono::steady_clock::time_point m_PrevSampleTime;

    // Thread safety
    mutable std::shared_mutex m_Mutex;
    std::shared_ptr<const GPUPublication> m_Publication = std::make_shared<const GPUPublication>();
    std::uint64_t m_PublicationVersion = 0;
    std::atomic<std::uint64_t> m_PublishedPublicationVersion{0};

    // Helper: compute snapshot from current/previous counters
    [[nodiscard]] GPUSnapshot
    computeSnapshot(const Platform::GPUCounters& current, const Platform::GPUCounters* previous, double timeDeltaSeconds) const;

    // Helper template: extract a field from GPU history and return as float vector
    template<typename FieldPtr> [[nodiscard]] std::vector<float> getHistoryField(std::string_view gpuId, FieldPtr field) const;
    // Underlying helper shared with getHistoryField(): a per-sample projection instead of a
    // plain member pointer, for accessors whose value depends on more than one field.
    template<typename Projection>
    [[nodiscard]] std::vector<float> getHistoryFieldByProjection(std::string_view gpuId, Projection project) const;
    void publish();

    // Size every history ring for m_MaxHistorySeconds at the fastest refresh cadence (caller holds m_Mutex).
    void applyHistoryCapacity();
    // Drop samples older than m_MaxHistorySeconds before nowSeconds, except the newest of them while a
    // newer sample remains, like HistoryUtils::discardBefore (#1016) (caller holds m_Mutex).
    void trimHistory(double nowSeconds);
};

} // namespace Domain
