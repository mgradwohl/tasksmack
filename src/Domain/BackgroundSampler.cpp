#include "BackgroundSampler.h"

#include "Domain/ISamplable.h"
#include "SamplingConfig.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Domain
{

namespace
{

constexpr auto EXCEPTION_LOG_THROTTLE = std::chrono::seconds(5);

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

BackgroundSampler::BackgroundSampler(SamplerConfig config)
    : m_Config{.interval = std::chrono::milliseconds(Sampling::clampRefreshInterval(config.interval.count())),
               .firstSampleAfterInterval = config.firstSampleAfterInterval}
{
    spdlog::debug("BackgroundSampler: created with {}ms interval", m_Config.interval.count());
}

// NOLINTNEXTLINE(bugprone-exception-escape) - spdlog logging in stop() may theoretically throw; acceptable in practice
BackgroundSampler::~BackgroundSampler()
{
    stop();
}

void BackgroundSampler::addSamplable(std::weak_ptr<ISamplable> samplable)
{
    std::scoped_lock const lock(m_SamplablesMutex);
    m_Samplables.push_back(std::move(samplable));
}

void BackgroundSampler::start()
{
    if (m_Running.load())
    {
        spdlog::warn("BackgroundSampler: already running");
        return;
    }

    spdlog::info("BackgroundSampler: starting with {}ms interval", m_Config.interval.count());
    m_Running.store(true);
    m_SamplerThread = std::jthread([this](const std::stop_token& st) { samplerLoop(st); });
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

    spdlog::debug("BackgroundSampler: stopped");
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

        std::vector<std::weak_ptr<ISamplable>> currentSamplables;
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

        for (const auto& weakSamplable : currentSamplables)
        {
            if (stopToken.stop_requested())
            {
                break;
            }

            // Lock the weak_ptr to get a temporary shared_ptr for the duration of this sample()
            // call. If the owner has already destroyed the samplable, lock() returns empty and
            // this entry is skipped rather than dereferencing a dangling pointer.
            const auto samplable = weakSamplable.lock();
            if (!samplable)
            {
                continue;
            }

            try
            {
                samplable->sample();
            }
            catch (const std::exception& ex)
            {
                logSamplerLoopException(ex.what(), startTime, nextExceptionLogTime, suppressedExceptionCount);
                hadException = true;
            }
            catch (...)
            {
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

        const auto nextSampleTime = startTime + currentInterval;
        std::unique_lock wakeLock(m_WakeMutex);
        auto deadline = nextSampleTime;
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
            // A deadline already past samples immediately.
            deadline = startTime + currentInterval;
        }
    }

    spdlog::debug("BackgroundSampler: thread exiting");
}

} // namespace Domain
