#include "BackgroundSampler.h"

#include "Domain/ISamplable.h"
#include "Platform/ThreadName.h"
#include "SamplingConfig.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
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
    ++timing.samples;
    if (duration > interval)
    {
        ++timing.overruns;
    }
}

void logSamplerLoopException(std::string_view message,
                             std::chrono::steady_clock::time_point now,
                             std::chrono::steady_clock::time_point& nextLogTime,
                             std::size_t& suppressedCount)
{
    if (now >= nextLogTime)
    {
        if (suppressedCount > 0)
        {
            spdlog::error(
                "BackgroundSampler: sampler loop exception repeated {} additional times; latest error: {}", suppressedCount, message);
        }
        else
        {
            spdlog::error("BackgroundSampler: exception in sampler loop: {}", message);
        }

        nextLogTime = now + EXCEPTION_LOG_THROTTLE;
        suppressedCount = 0;
        return;
    }

    ++suppressedCount;
}

} // namespace

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

BackgroundSampler::BackgroundSampler(SamplerConfig config)
    : m_Config{.interval = std::chrono::milliseconds(Sampling::clampRefreshInterval(config.interval.count())),
               .firstSampleAfterInterval = config.firstSampleAfterInterval,
               .threadName = std::move(config.threadName)}
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
    m_Running.store(true);
    std::string threadName;
    {
        const std::scoped_lock lock(m_ConfigMutex);
        threadName = m_Config.threadName;
    }
    m_SamplerThread = std::jthread(
        [this, name = std::move(threadName)](const std::stop_token& st)
        {
            if (!Platform::setCurrentThreadName(name))
            {
                spdlog::debug("BackgroundSampler: could not name thread '{}'", name);
            }
            samplerLoop(st);
        });
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
    // Called from stop(), so from the destructor too: nothing here may throw. No copy of metrics()
    // (its vector and strings allocate): only the scalars the line needs, under the lock, and only
    // when the line would be logged. spdlog formats into allocated buffers, and none of its calls are
    // noexcept, so all of it is guarded.
    try
    {
        if (!spdlog::should_log(spdlog::level::debug))
        {
            return;
        }
        SampleTiming pass;
        std::uint64_t backoffs = 0;
        {
            const std::scoped_lock lock(m_MetricsMutex);
            pass.samples = m_Metrics.pass.samples;
            pass.lastDuration = m_Metrics.pass.lastDuration;
            pass.maxDuration = m_Metrics.pass.maxDuration;
            pass.overruns = m_Metrics.pass.overruns;
            backoffs = m_Metrics.backoffs;
        }
        spdlog::debug("BackgroundSampler: stopped after {} passes; pass last/max {}/{} us, {} overruns, {} backoffs",
                      pass.samples,
                      pass.lastDuration.count(),
                      pass.maxDuration.count(),
                      pass.overruns,
                      backoffs);
    }
    catch (...) // NOLINT(bugprone-empty-catch) - a lost shutdown log line must not terminate the app
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

void BackgroundSampler::recordPass(const std::vector<Entry>& entries,
                                   const std::vector<std::optional<std::chrono::steady_clock::duration>>& durations,
                                   std::chrono::steady_clock::duration passDuration,
                                   std::chrono::milliseconds interval,
                                   std::chrono::steady_clock::time_point now)
{
    const bool overran = passDuration > interval;
    std::size_t slowest = entries.size();
    auto slowestDuration = std::chrono::steady_clock::duration::min();
    {
        const std::scoped_lock lock(m_MetricsMutex);
        if (m_Metrics.samplables.size() < entries.size())
        {
            m_Metrics.samplables.resize(entries.size());
        }
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            SampleTiming& timing = m_Metrics.samplables[i];
            if (timing.name.empty())
            {
                timing.name = entries[i].name;
            }
            // No duration: not called this pass (expired, or the sampler was stopping). A sample that
            // threw has one: it was timed like any other.
            if (const auto& duration = durations[i]; duration.has_value())
            {
                recordTiming(timing, *duration, interval);
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
                 (slowest < entries.size()) ? entries[slowest].name : std::string("none"));
    m_OverrunsSinceLog = 0;
    m_NextOverrunLogTime = now + OVERRUN_LOG_THROTTLE;
}

void BackgroundSampler::samplerLoop(const std::stop_token& stopToken)
{
    spdlog::debug("BackgroundSampler: thread started");
    auto nextExceptionLogTime = std::chrono::steady_clock::time_point::min();
    std::size_t suppressedExceptionCount = 0;
    bool skipFirstSample = false;
    {
        const std::scoped_lock lock(m_ConfigMutex);
        skipFirstSample = m_Config.firstSampleAfterInterval;
    }

    while (!stopToken.stop_requested())
    {
        auto startTime = std::chrono::steady_clock::now();
        bool hadException = false;

        std::vector<Entry> currentSamplables;
        if (skipFirstSample)
        {
            // Already seeded by the owner (SamplerConfig::firstSampleAfterInterval): this pass only waits.
            skipFirstSample = false;
        }
        else
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
                logSamplerLoopException(ex.what(), startTime, nextExceptionLogTime, suppressedExceptionCount);
                hadException = true;
            }
            catch (...)
            {
                durations[index] = std::chrono::steady_clock::now() - sampleStart;
                logSamplerLoopException("unknown exception", startTime, nextExceptionLogTime, suppressedExceptionCount);
                hadException = true;
            }
        }

        if (!hadException)
        {
            nextExceptionLogTime = std::chrono::steady_clock::time_point::min();
            suppressedExceptionCount = 0;
        }

        // Get current interval
        std::chrono::milliseconds currentInterval;
        {
            const std::scoped_lock lock(m_ConfigMutex);
            currentInterval = m_Config.interval;
        }

        const auto passEnd = std::chrono::steady_clock::now();
        const auto passDuration = passEnd - startTime;
        if (!currentSamplables.empty())
        {
            recordPass(currentSamplables, durations, passDuration, currentInterval, passEnd);
        }

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
            {
                const std::scoped_lock lock(m_ConfigMutex);
                currentInterval = m_Config.interval;
            }
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
