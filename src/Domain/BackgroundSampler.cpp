#include "BackgroundSampler.h"

#include <spdlog/spdlog.h>

#include <algorithm>
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

BackgroundSampler::BackgroundSampler(SamplerConfig config) : m_Config(config)
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
    m_RefreshRequested.store(true);
}

std::chrono::milliseconds BackgroundSampler::interval() const
{
    const std::scoped_lock lock(m_ConfigMutex);
    return m_Config.interval;
}

void BackgroundSampler::setInterval(std::chrono::milliseconds newInterval)
{
    const std::scoped_lock lock(m_ConfigMutex);
    m_Config.interval = newInterval;
    spdlog::info("BackgroundSampler: interval changed to {}ms", newInterval.count());
}

void BackgroundSampler::samplerLoop(const std::stop_token& stopToken)
{
    spdlog::debug("BackgroundSampler: thread started");
    auto nextExceptionLogTime = std::chrono::steady_clock::time_point::min();
    std::size_t suppressedExceptionCount = 0;

    while (!stopToken.stop_requested())
    {
        auto startTime = std::chrono::steady_clock::now();

        try
        {
            for (auto* samplable : m_Samplables)
            {
                if (stopToken.stop_requested())
                {
                    break;
                }
                samplable->sample();
            }

            nextExceptionLogTime = std::chrono::steady_clock::time_point::min();
            suppressedExceptionCount = 0;
        }
        catch (const std::exception& ex)
        {
            logSamplerLoopException(ex.what(), startTime, nextExceptionLogTime, suppressedExceptionCount);
        }
        catch (...)
        {
            logSamplerLoopException("unknown exception", startTime, nextExceptionLogTime, suppressedExceptionCount);
        }

        // Get current interval
        std::chrono::milliseconds currentInterval;
        {
            const std::scoped_lock lock(m_ConfigMutex);
            currentInterval = m_Config.interval;
        }

        // Calculate sleep time
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        auto sleepTime = currentInterval - std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);

        // Sleep in small increments to check for stop/refresh requests.
        // Consume a pending request with exchange(false) so a request issued while
        // sampling is not lost before we enter the sleep phase.
        constexpr auto checkInterval = std::chrono::milliseconds(50);
        bool refreshRequested = m_RefreshRequested.exchange(false);
        while (sleepTime > std::chrono::milliseconds(0) && !stopToken.stop_requested() && !refreshRequested)
        {
            auto sleepChunk = std::min(sleepTime, checkInterval);
            std::this_thread::sleep_for(sleepChunk);
            sleepTime -= sleepChunk;
            refreshRequested = m_RefreshRequested.exchange(false);
        }
    }

    spdlog::debug("BackgroundSampler: thread exiting");
}

} // namespace Domain
