#include "BackgroundSampler.h"

#include "SamplingConfig.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

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
    : m_Config{std::chrono::milliseconds(Sampling::clampRefreshInterval(config.interval.count()))}
{
    spdlog::debug("BackgroundSampler: created with {}ms interval", m_Config.interval.count());
}

// NOLINTNEXTLINE(bugprone-exception-escape) - spdlog logging in stop() may theoretically throw; acceptable in practice
BackgroundSampler::~BackgroundSampler()
{
    stop();
}

void BackgroundSampler::addSamplable(ISamplable* samplable)
{
    if (samplable != nullptr)
    {
        std::lock_guard lock(m_SamplablesMutex);
        m_Samplables.push_back(samplable);
    }
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
        const std::lock_guard lock(m_WakeMutex);
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
    requestRefresh();
}

void BackgroundSampler::samplerLoop(const std::stop_token& stopToken)
{
    spdlog::debug("BackgroundSampler: thread started");
    auto nextExceptionLogTime = std::chrono::steady_clock::time_point::min();
    std::size_t suppressedExceptionCount = 0;

    while (!stopToken.stop_requested())
    {
        auto startTime = std::chrono::steady_clock::now();
        bool hadException = false;

        std::vector<ISamplable*> currentSamplables;
        {
            std::lock_guard lock(m_SamplablesMutex);
            currentSamplables = m_Samplables;
        }

        for (auto* samplable : currentSamplables)
        {
            if (stopToken.stop_requested())
            {
                break;
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
        if (m_RefreshRequested)
        {
            m_RefreshRequested = false;
            continue;
        }

        m_WakeCondition.wait_until(wakeLock, stopToken, nextSampleTime, [this] { return m_RefreshRequested; });
        m_RefreshRequested = false;
    }

    spdlog::debug("BackgroundSampler: thread exiting");
}

} // namespace Domain
