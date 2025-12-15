#pragma once

#include "Platform/IProcessProbe.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace Domain
{

/// Configuration for background sampling.
struct SamplerConfig
{
    std::chrono::milliseconds interval{1000}; // 1 second default
};

/// Background sampler that runs probe enumeration on a separate thread.
/// Publishes snapshots via callback; thread-safe snapshot access.
class BackgroundSampler
{
  public:
    using SnapshotCallback = std::function<void(const std::vector<Platform::ProcessCounters>&, uint64_t)>;

    explicit BackgroundSampler(std::unique_ptr<Platform::IProcessProbe> probe, SamplerConfig config = {});
    ~BackgroundSampler();

    BackgroundSampler(const BackgroundSampler&) = delete;
    BackgroundSampler& operator=(const BackgroundSampler&) = delete;
    BackgroundSampler(BackgroundSampler&&) = delete;
    BackgroundSampler& operator=(BackgroundSampler&&) = delete;

    /// Start background sampling thread.
    void start();

    /// Stop background sampling thread (waits for completion).
    void stop();

    /// Check if sampler is running.
    [[nodiscard]] bool isRunning() const;

    /// Set callback for when new data arrives.
    void setCallback(SnapshotCallback callback);

    /// Get the probe capabilities.
    [[nodiscard]] const Platform::ProcessCapabilities& capabilities() const;

    /// Get ticks per second from probe.
    [[nodiscard]] long ticksPerSecond() const;

    /// Request an immediate refresh (next iteration).
    void requestRefresh();

    /// Get current sampling interval.
    [[nodiscard]] std::chrono::milliseconds interval() const;

    /// Set sampling interval (takes effect on next iteration).
    void setInterval(std::chrono::milliseconds interval);

  private:
    void samplerLoop(std::stop_token stopToken);

    std::unique_ptr<Platform::IProcessProbe> m_Probe;
    Platform::ProcessCapabilities m_Capabilities;
    SamplerConfig m_Config;

    std::jthread m_SamplerThread;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_RefreshRequested{false};

    mutable std::mutex m_CallbackMutex;
    SnapshotCallback m_Callback;

    mutable std::mutex m_ConfigMutex;
};

} // namespace Domain
