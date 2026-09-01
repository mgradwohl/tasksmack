#pragma once

#include "ISamplable.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace Domain
{

/// Configuration for background sampling.
struct SamplerConfig
{
    std::chrono::milliseconds interval{1000}; // 1 second default
};

/// Background sampler that runs sampling on a separate thread.
class BackgroundSampler
{
  public:
    explicit BackgroundSampler(SamplerConfig config = {});
    ~BackgroundSampler();

    BackgroundSampler(const BackgroundSampler&) = delete;
    BackgroundSampler& operator=(const BackgroundSampler&) = delete;
    BackgroundSampler(BackgroundSampler&&) = delete;
    BackgroundSampler& operator=(BackgroundSampler&&) = delete;

    /// Add a samplable object to be refreshed on the background thread.
    /// Stored as a weak_ptr: the sampler observes the object but never extends its lifetime,
    /// so the owner (a Panel) can destroy it at will without any destruction-order dependency
    /// on the sampler. A samplable whose owner has released it is silently skipped on the next
    /// sampling iteration rather than causing a use-after-free.
    void addSamplable(std::weak_ptr<ISamplable> samplable);

    /// Start background sampling thread.
    void start();

    /// Stop background sampling thread (waits for completion).
    void stop();

    /// Check if sampler is running.
    [[nodiscard]] bool isRunning() const;

    /// Request an immediate refresh (next iteration).
    void requestRefresh();

    /// Get current sampling interval.
    [[nodiscard]] std::chrono::milliseconds interval() const;

    /// Set sampling interval (takes effect on next iteration).
    void setInterval(std::chrono::milliseconds interval);

  private:
    void samplerLoop(const std::stop_token& stopToken);

    SamplerConfig m_Config;
    std::vector<std::weak_ptr<ISamplable>> m_Samplables;

    std::jthread m_SamplerThread;
    std::atomic<bool> m_Running{false};

    mutable std::mutex m_ConfigMutex;
    mutable std::mutex m_SamplablesMutex;
    std::mutex m_WakeMutex;
    std::condition_variable_any m_WakeCondition;
    bool m_RefreshRequested = false;
    bool m_IntervalChanged = false;
};

} // namespace Domain
