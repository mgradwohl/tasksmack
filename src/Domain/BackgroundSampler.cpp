#include "BackgroundSampler.h"

#include "Domain/ISamplable.h"
#include "Platform/ThreadApartment.h"
#include "Platform/ThreadName.h"
#include "SamplerTimingBoard.h"
#include "SamplingConfig.h"
#include "TimingHistogram.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Domain
{

namespace
{

constexpr auto EXCEPTION_LOG_THROTTLE = std::chrono::seconds(5);
/// A sustained overrun is logged at most this often, with the count since the last log (#1416).
constexpr auto OVERRUN_LOG_THROTTLE = std::chrono::seconds(30);

[[nodiscard]] std::chrono::microseconds toMicroseconds(std::chrono::steady_clock::duration duration) noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(duration);
}

void recordTiming(SampleTiming& timing, std::chrono::steady_clock::duration duration, std::chrono::milliseconds interval) noexcept
{
    timing.lastDuration = toMicroseconds(duration);
    timing.maxDuration = std::max(timing.maxDuration, timing.lastDuration);
    timing.histogram.record(timing.lastDuration);
    ++timing.samples;
    if (duration > interval)
    {
        ++timing.overruns;
    }
}

/// One timing as formatTimingSummary() lists it, appended to @p out.
void appendTiming(std::string& out, const SampleTiming& timing)
{
    const TimingHistogram& histogram = timing.histogram;
    std::format_to(std::back_inserter(out),
                   "{} n={} mean={} p50={} p95={} max={} overruns={}",
                   timing.name,
                   histogram.count(),
                   histogram.mean().count(),
                   histogram.percentile(0.50).count(),
                   histogram.percentile(0.95).count(),
                   histogram.max().count(),
                   timing.overruns);
}

} // namespace

std::string formatTimingSummary(std::string_view sampler, std::string_view event, const SamplerMetrics& metrics)
{
    std::string line = std::format(
        "BackgroundSampler {}: {} after {} passes, {} backoffs; timings in us: ", sampler, event, metrics.pass.samples, metrics.backoffs);
    appendTiming(line, metrics.pass);
    for (const SampleTiming& timing : metrics.samplables)
    {
        line += "; ";
        appendTiming(line, timing);
    }
    return line;
}

void BackgroundSampler::logLoopException(std::string_view message,
                                         std::chrono::steady_clock::time_point now,
                                         ExceptionLogThrottle& throttle) noexcept
{
    // Called from catch handlers, the sampler thread's last line of defence: a failure to log (spdlog
    // formats into allocated buffers) must not escape the thread and terminate the app (#2317).
    try
    {
        if (now >= throttle.nextLogTime)
        {
            if (throttle.suppressedCount > 0)
            {
                spdlog::error("BackgroundSampler: sampler loop exception repeated {} additional times; latest error: {}",
                              throttle.suppressedCount,
                              message);
            }
            else
            {
                spdlog::error("BackgroundSampler: exception in sampler loop: {}", message);
            }

            throttle.nextLogTime = now + EXCEPTION_LOG_THROTTLE;
            throttle.suppressedCount = 0;
            return;
        }

        ++throttle.suppressedCount;
    }
    catch (...) // NOLINT(bugprone-empty-catch) - a lost log line must not terminate the app
    {}
}

std::chrono::steady_clock::time_point nextSampleTime(std::chrono::steady_clock::time_point passStart,
                                                     std::chrono::steady_clock::duration passDuration,
                                                     std::chrono::milliseconds interval) noexcept
{
    using Duration = std::chrono::steady_clock::duration;
    if (passDuration <= interval)
    {
        return passStart + interval;
    }
    // Overran: rest as long as the pass took (so longer than an interval), capped so a stalled probe
    // can't stop sampling for longer than the slowest supported interval.
    constexpr Duration MAX_GAP = std::chrono::milliseconds(Sampling::REFRESH_INTERVAL_MAX_MS);
    const Duration gap = std::min<Duration>(passDuration, MAX_GAP);
    return passStart + passDuration + gap;
}

void BackgroundSampler::runStartHook(const std::function<void()>& hook, std::string_view threadName) noexcept
{
    if (!hook)
    {
        return;
    }
    // The hook builds probes and seeds models: slow, and allowed to fail. A failure is the owner's to
    // notice (a model left without a probe publishes nothing); sampling goes on, so stop() and the
    // metrics behave as for any other run (#2731).
    try
    {
        const auto started = std::chrono::steady_clock::now();
        hook();
        spdlog::info("BackgroundSampler: start hook ran on {} in {} ms",
                     threadName,
                     std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    }
    catch (const std::exception& ex)
    {
        try
        {
            spdlog::error("BackgroundSampler: start hook failed on {}: {}", threadName, ex.what());
        }
        catch (...) // NOLINT(bugprone-empty-catch) - a lost log line must not terminate the app
        {}
    }
    catch (...)
    {
        try
        {
            spdlog::error("BackgroundSampler: start hook failed on {}: unknown exception", threadName);
        }
        catch (...) // NOLINT(bugprone-empty-catch) - a lost log line must not terminate the app
        {}
    }
}

BackgroundSampler::BackgroundSampler(SamplerConfig config)
    : m_Config{.interval = std::chrono::milliseconds(Sampling::clampRefreshInterval(config.interval.count())),
               .firstSampleAfterInterval = config.firstSampleAfterInterval,
               .threadName = std::move(config.threadName),
               .onThreadStart = std::move(config.onThreadStart),
               .multiThreadedApartment = config.multiThreadedApartment,
               .timingSummaryPeriod = config.timingSummaryPeriod}
{
    spdlog::debug("BackgroundSampler: created with {}ms interval", m_Config.interval.count());
}

// NOLINTNEXTLINE(bugprone-exception-escape) - spdlog logging in stop() may theoretically throw; acceptable in practice
BackgroundSampler::~BackgroundSampler()
{
    stop();
}

void BackgroundSampler::addSamplable(std::weak_ptr<ISamplable> samplable, std::string name)
{
    std::scoped_lock const lock(m_SamplablesMutex);
    if (name.empty())
    {
        name = "samplable " + std::to_string(m_Samplables.size());
    }
    m_Samplables.push_back({.samplable = std::move(samplable), .name = std::move(name)});
}

void BackgroundSampler::start()
{
    if (m_Running.load())
    {
        spdlog::warn("BackgroundSampler: already running");
        return;
    }

    spdlog::info("BackgroundSampler: starting with {}ms interval", m_Config.interval.count());
    {
        const std::scoped_lock lock(m_MetricsMutex);
        m_Metrics = SamplerMetrics{};
    }
    m_NextOverrunLogTime = std::chrono::steady_clock::time_point::min();
    m_OverrunsSinceLog = 0;
    // Everything that can throw before the thread exists comes before the state changes below.
    std::string threadName;
    std::function<void()> startHook;
    bool multiThreadedApartment = false;
    {
        const std::scoped_lock lock(m_ConfigMutex);
        threadName = m_Config.threadName;
        startHook = m_Config.onThreadStart;
        multiThreadedApartment = m_Config.multiThreadedApartment;
    }
    m_Running.store(true);
    try
    {
        m_SamplerThread = std::jthread(
            [this, name = std::move(threadName), hook = std::move(startHook), multiThreadedApartment](const std::stop_token& st) noexcept
            {
                // Nothing may escape a thread's entry point: it would std::terminate the app. Each pass
                // contains its own failures (samplerLoop()), so this only catches what is left -- the
                // wait between passes, a log line -- and ends sampling rather than the app (#2317).
                try
                {
                    if (!Platform::setCurrentThreadName(name))
                    {
                        spdlog::debug("BackgroundSampler: could not name thread '{}'", name);
                    }
                    // For the thread's whole life, so it outlives everything the hook and the passes
                    // create; an empty optional where the owner didn't ask for it (#2731).
                    std::optional<Platform::MultiThreadedApartment> apartment;
                    if (multiThreadedApartment)
                    {
                        apartment.emplace();
                    }
                    runStartHook(hook, name);
                    samplerLoop(st, name);
                }
                catch (...)
                {
                    try
                    {
                        spdlog::critical("BackgroundSampler: sampler thread stopped by an unexpected exception");
                    }
                    catch (...) // NOLINT(bugprone-empty-catch) - a lost log line must not terminate the app
                    {}
                }
            });
    }
    catch (...)
    {
        // No thread was started (std::system_error, std::bad_alloc): back to stopped, so isRunning() is
        // false and start() can be called again.
        m_Running.store(false);
        throw;
    }
}

void BackgroundSampler::stop()
{
    if (!m_Running.load())
    {
        return;
    }

    spdlog::info("BackgroundSampler: stopping");
    m_Running.store(false);

    if (m_SamplerThread.joinable())
    {
        m_SamplerThread.request_stop();
        m_WakeCondition.notify_all();
        m_SamplerThread.join();
    }

    logStopSummary();
}

void BackgroundSampler::logStopSummary() const noexcept
{
    // Named by its thread, so each sampler's line can be told apart in the log (#3153). Called from
    // stop(), so from the destructor too: a failed copy of the name loses only the line.
    try
    {
        if (!spdlog::should_log(spdlog::level::debug))
        {
            return;
        }
        std::string name;
        {
            const std::scoped_lock lock(m_ConfigMutex);
            name = m_Config.threadName;
        }
        logTimingSummary(name, "stopped");
    }
    catch (...) // NOLINT(bugprone-empty-catch) - a lost shutdown log line must not terminate the app
    {}
}

void BackgroundSampler::logTimingSummary(std::string_view name, std::string_view event) const noexcept
{
    // Called from stop(), so from the destructor too, and from the sampler thread after a pass: nothing
    // here may throw. No copy of metrics() (its vector and strings allocate): the line is formatted
    // under the lock, and only when it would be logged. spdlog formats into allocated buffers, and none
    // of its calls are noexcept, so all of it is guarded.
    try
    {
        if (!spdlog::should_log(spdlog::level::debug))
        {
            return;
        }
        std::string line;
        {
            const std::scoped_lock lock(m_MetricsMutex);
            line = formatTimingSummary(name, event, m_Metrics);
        }
        spdlog::debug("{}", line);
    }
    catch (...) // NOLINT(bugprone-empty-catch) - a lost summary line must not terminate the app
    {}
}

bool BackgroundSampler::isRunning() const
{
    return m_Running.load();
}

void BackgroundSampler::requestRefresh()
{
    {
        const std::scoped_lock lock(m_WakeMutex);
        m_RefreshRequested = true;
    }
    m_WakeCondition.notify_all();
}

std::chrono::milliseconds BackgroundSampler::interval() const
{
    const std::scoped_lock lock(m_ConfigMutex);
    return m_Config.interval;
}

void BackgroundSampler::setInterval(std::chrono::milliseconds newInterval)
{
    const auto clampedInterval = std::chrono::milliseconds(Sampling::clampRefreshInterval(newInterval.count()));
    {
        const std::scoped_lock lock(m_ConfigMutex);
        m_Config.interval = clampedInterval;
    }
    spdlog::info("BackgroundSampler: interval changed to {}ms", clampedInterval.count());
    {
        const std::scoped_lock lock(m_WakeMutex);
        m_IntervalChanged = true;
    }
    m_WakeCondition.notify_all();
}

SamplerMetrics BackgroundSampler::metrics() const
{
    const std::scoped_lock lock(m_MetricsMutex);
    return m_Metrics;
}

void BackgroundSampler::recordPass(std::string_view name,
                                   const std::vector<Entry>& entries,
                                   const std::vector<std::optional<std::chrono::steady_clock::duration>>& durations,
                                   std::chrono::steady_clock::duration passDuration,
                                   std::chrono::milliseconds interval,
                                   std::chrono::steady_clock::time_point now)
{
    const bool overran = passDuration > interval;
    std::size_t slowest = entries.size();
    auto slowestDuration = std::chrono::steady_clock::duration::min();
    TimingHistogram passHistogram;
    std::uint64_t passOverruns = 0;
    std::uint64_t backoffs = 0;
    {
        const std::scoped_lock lock(m_MetricsMutex);
        // Everything that allocates (the resize, the name copies) comes before any timing is recorded,
        // so a std::bad_alloc here leaves none of this pass half-recorded: the pass is simply not
        // counted, and the names are filled in by the next pass that gets this far (#2317).
        if (m_Metrics.samplables.size() < entries.size())
        {
            m_Metrics.samplables.resize(entries.size());
        }
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            if (SampleTiming& timing = m_Metrics.samplables[i]; timing.name.empty())
            {
                timing.name = entries[i].name;
            }
        }
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            // No duration: not called this pass (expired, or the sampler was stopping). A sample that
            // threw has one: it was timed like any other.
            if (const auto& duration = durations[i]; duration.has_value())
            {
                recordTiming(m_Metrics.samplables[i], *duration, interval);
                if (*duration > slowestDuration)
                {
                    slowest = i;
                    slowestDuration = *duration;
                }
            }
        }
        recordTiming(m_Metrics.pass, passDuration, interval);
        if (overran)
        {
            ++m_Metrics.backoffs;
        }
        passHistogram = m_Metrics.pass.histogram;
        passOverruns = m_Metrics.pass.overruns;
        backoffs = m_Metrics.backoffs;
    }
    // Published outside the metrics lock, a fixed-size copy: what the Render Metrics overlay shows (#3153).
    SamplerTimingBoard::global().publish(name, passHistogram, passOverruns, backoffs);

    if (now >= m_NextTimingSummaryTime)
    {
        logTimingSummary(name, "running");
        m_NextTimingSummaryTime = now + m_TimingSummaryPeriod;
    }

    if (!overran)
    {
        return;
    }
    ++m_OverrunsSinceLog;
    if (now < m_NextOverrunLogTime)
    {
        return;
    }
    spdlog::warn("BackgroundSampler: a sampling pass took {} ms against a {} ms interval ({} overruns since the last report); "
                 "slowest: {}; backing off",
                 std::chrono::duration_cast<std::chrono::milliseconds>(passDuration).count(),
                 interval.count(),
                 m_OverrunsSinceLog,
                 (slowest < entries.size()) ? std::string_view{entries[slowest].name} : std::string_view{"none"});
    m_OverrunsSinceLog = 0;
    m_NextOverrunLogTime = now + OVERRUN_LOG_THROTTLE;
}

BackgroundSampler::PassTiming BackgroundSampler::runPass(const std::stop_token& stopToken,
                                                         std::string_view name,
                                                         bool samplePass,
                                                         std::chrono::steady_clock::time_point startTime,
                                                         ExceptionLogThrottle& exceptionLog)
{
    bool hadException = false;

    std::vector<Entry> currentSamplables;
    if (samplePass)
    {
        std::scoped_lock const lock(m_SamplablesMutex);
        currentSamplables = m_Samplables;
    }

    // How long each samplable's sample() took this pass, whether it returned or threw; empty for
    // one not called (expired, or the sampler was stopping) (#1416).
    std::vector<std::optional<std::chrono::steady_clock::duration>> durations(currentSamplables.size());
    for (std::size_t index = 0; index < currentSamplables.size(); ++index)
    {
        if (stopToken.stop_requested())
        {
            break;
        }

        // Lock the weak_ptr to get a temporary shared_ptr for the duration of this sample()
        // call. If the owner has already destroyed the samplable, lock() returns empty and
        // this entry is skipped rather than dereferencing a dangling pointer.
        const auto samplable = currentSamplables[index].samplable.lock();
        if (!samplable)
        {
            continue;
        }

        const auto sampleStart = std::chrono::steady_clock::now();
        try
        {
            samplable->sample();
            durations[index] = std::chrono::steady_clock::now() - sampleStart;
        }
        // A sample that throws still took its time: timed too, so a slow failing model is
        // counted (samples, overruns) and named as the slowest rather than hidden (#1416).
        catch (const std::exception& ex)
        {
            durations[index] = std::chrono::steady_clock::now() - sampleStart;
            logLoopException(ex.what(), startTime, exceptionLog);
            hadException = true;
        }
        catch (...)
        {
            durations[index] = std::chrono::steady_clock::now() - sampleStart;
            logLoopException("unknown exception", startTime, exceptionLog);
            hadException = true;
        }
    }

    const auto passEnd = std::chrono::steady_clock::now();
    const PassTiming timing{.duration = passEnd - startTime, .interval = interval()};
    if (!currentSamplables.empty())
    {
        recordPass(name, currentSamplables, durations, timing.duration, timing.interval, passEnd);
    }

    if (!hadException)
    {
        exceptionLog = {};
    }
    return timing;
}

void BackgroundSampler::samplerLoop(const std::stop_token& stopToken, std::string_view name)
{
    spdlog::debug("BackgroundSampler: thread started");
    ExceptionLogThrottle exceptionLog;
    bool skipFirstSample = false;
    {
        const std::scoped_lock lock(m_ConfigMutex);
        skipFirstSample = m_Config.firstSampleAfterInterval;
        m_TimingSummaryPeriod = m_Config.timingSummaryPeriod;
    }
    m_NextTimingSummaryTime = std::chrono::steady_clock::now() + m_TimingSummaryPeriod;

    while (!stopToken.stop_requested())
    {
        const auto startTime = std::chrono::steady_clock::now();
        // Already seeded by the owner (SamplerConfig::firstSampleAfterInterval): the first pass only waits.
        const bool samplePass = !std::exchange(skipFirstSample, false);

        PassTiming timing;
        try
        {
            timing = runPass(stopToken, name, samplePass, startTime, exceptionLog);
        }
        // The pass's own bookkeeping failed -- a std::bad_alloc copying the registrations, sizing the
        // durations or recording the metrics; each sample() has its own catch in runPass() (#2317).
        // The models keep their last snapshots and this pass's metrics are skipped. The wait below
        // keeps the normal cadence from this pass's start, so a persistent failure is retried once
        // an interval, never in a busy loop.
        catch (const std::exception& ex)
        {
            timing = {.duration = std::chrono::steady_clock::now() - startTime, .interval = interval()};
            logLoopException(ex.what(), startTime, exceptionLog);
        }
        catch (...)
        {
            timing = {.duration = std::chrono::steady_clock::now() - startTime, .interval = interval()};
            logLoopException("unknown exception", startTime, exceptionLog);
        }
        const auto passDuration = timing.duration;
        auto currentInterval = timing.interval;

        // The fixed cadence, or a bounded backoff after a pass that overran its interval (#1416).
        std::unique_lock wakeLock(m_WakeMutex);
        auto deadline = nextSampleTime(startTime, passDuration, currentInterval);
        while (!stopToken.stop_requested())
        {
            m_WakeCondition.wait_until(wakeLock, stopToken, deadline, [this] { return m_RefreshRequested || m_IntervalChanged; });

            if (stopToken.stop_requested())
            {
                break;
            }

            if (m_RefreshRequested)
            {
                m_RefreshRequested = false;
                // A refresh forced right after a sample -- a settings change landing just after a
                // scheduled one -- would sample milliseconds later, giving the models no usable
                // deltas: Total 0%/Idle 0% CPU and false 0 B/s rates that stay on the charts for the
                // whole window. Hold it until at least the fastest supported interval has passed
                // since this sample started (#1102).
                const auto earliest = startTime + std::chrono::milliseconds(Sampling::REFRESH_INTERVAL_MIN_MS);
                if (std::chrono::steady_clock::now() < earliest)
                {
                    m_WakeCondition.wait_until(wakeLock, stopToken, earliest, [] { return false; });
                }
                break;
            }

            if (!m_IntervalChanged)
            {
                break;
            }

            m_IntervalChanged = false;
            currentInterval = interval();
            // Rebase on when this sample started, not on now: resetting the wait on every change
            // let repeated changes (the interaction throttle toggling during a drag) postpone
            // sampling indefinitely, and a faster interval took a whole interval to apply (#1118).
            // A deadline already past samples immediately. An overrunning pass keeps its backoff,
            // recomputed for the new interval (#1416).
            deadline = nextSampleTime(startTime, passDuration, currentInterval);
        }
    }

    spdlog::debug("BackgroundSampler: thread exiting");
}

} // namespace Domain
