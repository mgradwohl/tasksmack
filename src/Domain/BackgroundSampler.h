#pragma once

#include "ISamplable.h"
#include "Platform/ThreadName.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Domain
{

/// Configuration for background sampling.
struct SamplerConfig
{
    std::chrono::milliseconds interval{1000}; // 1 second default

    /// The owner has just taken a sample itself (a synchronous seed before start(), or one in
    /// onThreadStart), so wait one interval before the first background sample. Sampling again straight
    /// away gave deltas over a few ms: 0% CPU and 0 B/s points at every startup (#1102).
    bool firstSampleAfterInterval = false;

    /// OS name of the sampler thread, so per-thread tools (pidstat -t, top -H, perf, WPA) can tell
    /// the process and system samplers apart. Linux keeps the first 15 bytes.
    std::string threadName{Platform::SAMPLER_THREAD_NAME};

    /// The start hook (#2731): run once per start(), on the sampler thread, after it is named and in
    /// its apartment (multiThreadedApartment) and before its first pass. The owner builds its probes
    /// and seeds its models here, so the UI thread never constructs a probe or blocks on a first read,
    /// and what a probe creates (PDH's providers, DXGI) belongs to the sampler thread. The hook runs
    /// with the sampler already running: stop() waits for it, requestRefresh() and setInterval() are
    /// taken after it. One that throws is logged and sampling goes on, with whatever it managed to set
    /// up; a model it left without a probe samples nothing.
    // NOLINTNEXTLINE(readability-redundant-member-init) - lets the tests' designated initializers leave it out
    std::function<void()> onThreadStart{};

    /// Windows: the sampler thread joins the multithreaded COM apartment for its life
    /// (Platform::MultiThreadedApartment), so the COM objects its probes create live there, not in the
    /// UI thread's STA where every later call would marshal through the UI's message loop (#2731).
    /// Elsewhere nothing. Off for samplers whose probes bring their own apartment (a ComScope).
    bool multiThreadedApartment = false;
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
    /// and start() can be called again.
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

    /// Sample durations and overruns since start() (thread-safe copy; #1416). The same figures the
    /// stop summary logs; read by the tests that check the pass timing and the overrun backoff.
    [[nodiscard]] SamplerMetrics metrics() const;

  private:
    struct Entry
    {
        std::weak_ptr<ISamplable> samplable;
        std::string name;
    };

    /// What one pass took, and the interval it was measured against.
    struct PassTiming
    {
        std::chrono::steady_clock::duration duration{};
        std::chrono::milliseconds interval{};
    };

    /// Rate-limits the sampler thread's exception log: one line per EXCEPTION_LOG_THROTTLE, with the
    /// count of those suppressed since.
    struct ExceptionLogThrottle
    {
        std::chrono::steady_clock::time_point nextLogTime = std::chrono::steady_clock::time_point::min();
        std::size_t suppressedCount = 0;
    };

    /// Runs the owner's start hook (SamplerConfig::onThreadStart), if any, on the sampler thread before
    /// its first pass, timing it and containing what it throws (#2731). Never throws.
    static void runStartHook(const std::function<void()>& hook, std::string_view threadName) noexcept;
    void samplerLoop(const std::stop_token& stopToken);
    /// One sampling pass: every samplable's sample() (each failure caught and logged), then the pass's
    /// metrics. Throws (std::bad_alloc) only from the pass's own bookkeeping, which samplerLoop()
    /// contains (#2317). `samplePass` false: the pass samples nothing and only waits.
    [[nodiscard]] PassTiming runPass(const std::stop_token& stopToken,
                                     bool samplePass,
                                     std::chrono::steady_clock::time_point startTime,
                                     ExceptionLogThrottle& exceptionLog);
    /// Log one of the sampler thread's exceptions, rate-limited by `throttle`. Never throws: it runs in
    /// the catch handlers that keep exceptions from escaping the thread (#2317).
    static void
    logLoopException(std::string_view message, std::chrono::steady_clock::time_point now, ExceptionLogThrottle& throttle) noexcept;
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
