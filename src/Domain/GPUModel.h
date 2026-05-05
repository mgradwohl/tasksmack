#pragma once

#include "Domain/GPUSnapshot.h"
#include "Domain/History.h"
#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Domain
{

// GPU history capacity: 5 minutes at 1 second intervals = 300 samples
inline constexpr size_t GPU_HISTORY_CAPACITY = 300;

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

class GPUModel
{
  public:
    explicit GPUModel(std::unique_ptr<Platform::IGPUProbe> probe);
    ~GPUModel() = default;

    GPUModel(const GPUModel&) = delete;
    GPUModel& operator=(const GPUModel&) = delete;
    GPUModel(GPUModel&&) = delete;
    GPUModel& operator=(GPUModel&&) = delete;

    // Refresh metrics (called by sampler thread)
    void refresh();

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

    // Get per-GPU timestamps (only samples where the GPU was present).
    // Length matches the per-GPU history vectors (utilizationHistory, etc.).
    [[nodiscard]] std::vector<double> historyTimestamps(std::string_view gpuId) const;

    // GPU info (static, rarely changes)
    [[nodiscard]] std::vector<Platform::GPUInfo> gpuInfo() const;

    // Capabilities
    [[nodiscard]] Platform::GPUCapabilities capabilities() const;

    // Per-process GPU counters (called by ProcessModel to enrich process snapshots)
    [[nodiscard]] std::vector<Platform::ProcessGPUCounters> readProcessGPUCounters() const;

    // Configuration: Set PDH instance refresh interval (Windows-only)
    void setInstanceRefreshInterval(std::chrono::seconds interval);

  private:
    std::unique_ptr<Platform::IGPUProbe> m_Probe;
    std::vector<Platform::GPUInfo> m_GPUInfo;

    // Current snapshots per GPU
    using SnapshotMap = std::unordered_map<std::string, GPUSnapshot, TransparentStringHash, TransparentStringEqual>;
    using HistoryMap =
        std::unordered_map<std::string, History<GPUSnapshot, GPU_HISTORY_CAPACITY>, TransparentStringHash, TransparentStringEqual>;
    using CounterMap = std::unordered_map<std::string, Platform::GPUCounters, TransparentStringHash, TransparentStringEqual>;

    SnapshotMap m_Snapshots;

    // History buffers per GPU
    HistoryMap m_Histories;

    // Timestamps for history data
    std::vector<double> m_HistoryTimestamps;

    // Previous counters for rate calculation
    CounterMap m_PrevCounters;
    std::chrono::steady_clock::time_point m_PrevSampleTime;

    // Thread safety
    mutable std::shared_mutex m_Mutex;

    // Helper: compute snapshot from current/previous counters
    [[nodiscard]] GPUSnapshot
    computeSnapshot(const Platform::GPUCounters& current, const Platform::GPUCounters* previous, double timeDeltaSeconds) const;

    // Helper template: extract a field from GPU history and return as float vector
    template<typename FieldPtr> [[nodiscard]] std::vector<float> getHistoryField(std::string_view gpuId, FieldPtr field) const;
};

} // namespace Domain
