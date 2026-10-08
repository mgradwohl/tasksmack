#pragma once

#include "ISamplable.h"
#include "Platform/ThreadName.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace Domain
{

/// Configuration for background sampling.
struct SamplerConfig
{
    std::chrono::milliseconds interval{1000}; // 1 second default

    /// The owner has just taken a sample itself (a synchronous seed before start()), so wait one
    /// interval before the first background sample. Sampling again straight away gave deltas over a
    /// few ms: 0% CPU and 0 B/s points at every startup (#1102).
    bool firstSampleAfterInterval = false;

    /// OS name of the sampler thread, so per-thread tools (pidstat -t, top -H, perf, WPA) can tell
    /// the process and system samplers apart. Linux keeps the first 15 bytes.
    std::string threadName{Platform::SAMPLER_THREAD_NAME};
};

/// How long samples take, for one samplable or for a whole sampling pass (#1416).
struct SampleTiming
{
    std::string name;                          ///< The samplable's name (addSamplable()), or "pass" for whole passes
    std::chrono::microseconds lastDuration{0}; ///< The most recent sample
    std::chrono::microseconds maxDuration{0};  ///< The slowest sample since start()
    std::uint64_t samples = 0;                 ///< Samples timed since start()
    std::uint64_t overruns = 0;                ///< Samples that took longer than the sampling interval
};

/// A BackgroundSampler's timing since start(): per samplable and per pass (#1416).
struct SamplerMetrics
{
    SampleTiming pass{.name = "pass"};    ///< Every samplable of one pass, back to back
    std::vector<SampleTiming> samplables; ///< In addSamplable() order
    std::uint64_t backoffs = 0;           ///< Passes that overran the interval, so the next waited longer (see nextSampleTime())
};

/// When the pass after one that started at `passStart` and took `passDuration` should start (#1416).
///
/// A pass that fits its interval keeps the fixed cadence: passStart + interval. One that overruns
/// it doesn't start the next straight away (that ran passes back to back, a core busy exactly when
/// the system was already slow): it waits after finishing for as long as the pass took, so sampling
/// takes at most about half a core, but never longer than REFRESH_INTERVAL_MAX_MS, so it never
/// stalls. The fixed cadence resumes with the first pass that fits again.
[[nodiscard]] std::chrono::steady_clock::time_point nextSampleTime(std::chrono::steady_clock::time_point passStart,
                                                                   std::chrono::steady_clock::duration passDuration,
                                                                   std::chrono::milliseconds interval) noexcept;

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
    /// `name` labels its timing in metrics() and the overrun log ("samplable N" when empty).
    void addSamplable(std::weak_ptr<ISamplable> samplable, std::string name = {});

    /// Start background sampling thread. If the thread can't be created (std::system_error,
    /// std::bad_alloc), the exception propagates and the sampler is left stopped: isRunning() false,
    /// hasThreadExited() true, and start() can be called again.
    void start();

    /// Stop background sampling thread (waits for completion).
    void stop();

    /// Ask the sampling thread to stop without waiting for it (#801), for an owner on the UI thread
    /// that must not block on a slow sample in flight (e.g. a panel whose tab was just left).
    ///
    /// Contract:
    /// - Never joins and never blocks: it only requests the stop and wakes the thread. A sample in
    ///   flight runs to completion on the sampler thread; no new sample starts after it.
    /// - The owner must keep this object alive until hasThreadExited() is true, or until it destroys
    ///   it. Destruction (and stop()) still joins, so destroying it before the thread has exited
    ///   waits for the sample in flight, exactly as stop() always has; afterwards neither waits.
    /// - isRunning() stays true until stop() or destruction, so start() is ignored until then. To
    ///   sample again straight away, create a new sampler.
    /// - Before start() it does nothing.
    void requestStop() noexcept;

    /// Whether the sampling thread has returned, so stop() and the destructor will not wait. True
    /// before start() too.
    [[nodiscard]] bool hasThreadExited() const noexcept;

    /// Check if sampler is running.
    [[nodiscard]] bool isRunning() const;

    /// Request an immediate refresh (next iteration).
    void requestRefresh();

    /// Get current sampling interval.
    [[nodiscard]] std::chrono::milliseconds interval() const;

    /// Set sampling interval (takes effect on next iteration).
    void setInterval(std::chrono::milliseconds interval);

    /// Sample durations and overruns since start() (thread-safe copy; #1416).
    [[nodiscard]] SamplerMetrics metrics() const;

  private:
    struct Entry
    {
        std::weak_ptr<ISamplable> samplable;
        std::string name;
    };

    void samplerLoop(const std::stop_token& stopToken);
    /// stop()'s debug summary line. Never throws (stop() runs from the destructor), and does nothing
    /// unless debug logging is on.
    void logStopSummary() const noexcept;
    /// Fold one pass's timings into m_Metrics, and log a sustained overrun (rate-limited).
    void recordPass(const std::vector<Entry>& entries,
                    const std::vector<std::optional<std::chrono::steady_clock::duration>>& durations,
                    std::chrono::steady_clock::duration passDuration,
                    std::chrono::milliseconds interval,
                    std::chrono::steady_clock::time_point now);

    SamplerConfig m_Config;
    std::vector<Entry> m_Samplables;

    std::jthread m_SamplerThread;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_ThreadExited{true};

    mutable std::mutex m_ConfigMutex;
    mutable std::mutex m_SamplablesMutex;
    std::mutex m_WakeMutex;
    std::condition_variable_any m_WakeCondition;
    bool m_RefreshRequested = false;
    bool m_IntervalChanged = false;

    mutable std::mutex m_MetricsMutex;
    SamplerMetrics m_Metrics;                                   // guarded by m_MetricsMutex
    std::chrono::steady_clock::time_point m_NextOverrunLogTime; // sampler thread only
    std::uint64_t m_OverrunsSinceLog = 0;                       // sampler thread only
};

} // namespace Domain
