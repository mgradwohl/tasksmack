#pragma once

#include "Domain/GPUSnapshot.h"
#include "Domain/History.h"
#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <chrono>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace Domain
{

// GPU history capacity: 5 minutes at 1 second intervals = 300 samples
inline constexpr size_t GPU_HISTORY_CAPACITY = 300;

class GPUModel
{
  public:
    explicit GPUModel(std::unique_ptr<Platform::IGPUProbe> probe);

    // Refresh metrics (called by sampler thread)
    void refresh();

    // Get current snapshots (thread-safe)
    [[nodiscard]] std::vector<GPUSnapshot> snapshots() const;

    // Get history for specific GPU
    [[nodiscard]] const History<GPUSnapshot, GPU_HISTORY_CAPACITY>& history(const std::string& gpuId) const;

    // GPU info (static, rarely changes)
    [[nodiscard]] std::vector<Platform::GPUInfo> gpuInfo() const;

    // Capabilities
    [[nodiscard]] Platform::GPUCapabilities capabilities() const;

  private:
    std::unique_ptr<Platform::IGPUProbe> m_Probe;
    std::vector<Platform::GPUInfo> m_GPUInfo;

    // Current snapshots per GPU
    std::unordered_map<std::string, GPUSnapshot> m_Snapshots;

    // History buffers per GPU
    std::unordered_map<std::string, History<GPUSnapshot, GPU_HISTORY_CAPACITY>> m_Histories;

    // Previous counters for rate calculation
    std::unordered_map<std::string, Platform::GPUCounters> m_PrevCounters;
    std::chrono::steady_clock::time_point m_PrevSampleTime;

    // Thread safety
    mutable std::shared_mutex m_Mutex;

    // Helper: compute snapshot from current/previous counters
    [[nodiscard]] GPUSnapshot
    computeSnapshot(const Platform::GPUCounters& current, const Platform::GPUCounters* previous, double timeDeltaSeconds) const;
};

} // namespace Domain
