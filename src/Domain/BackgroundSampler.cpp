#include "BackgroundSampler.h"

#include <spdlog/spdlog.h>

namespace Domain
{

BackgroundSampler::BackgroundSampler(std::unique_ptr<Platform::IProcessProbe> probe, SamplerConfig config)
    : m_Probe(std::move(probe)), m_Capabilities(m_Probe->capabilities()), m_Config(config)
{
    spdlog::debug("BackgroundSampler: created with {}ms interval", m_Config.interval.count());
}

BackgroundSampler::~BackgroundSampler()
{
    stop();
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
    m_SamplerThread = std::jthread([this](std::stop_token st) { samplerLoop(st); });
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

void BackgroundSampler::setCallback(SnapshotCallback callback)
{
    std::lock_guard lock(m_CallbackMutex);
    m_Callback = std::move(callback);
}

const Platform::ProcessCapabilities& BackgroundSampler::capabilities() const
{
    return m_Capabilities;
}

long BackgroundSampler::ticksPerSecond() const
{
    return m_Probe->ticksPerSecond();
}

void BackgroundSampler::requestRefresh()
{
    m_RefreshRequested.store(true);
}

std::chrono::milliseconds BackgroundSampler::interval() const
{
    std::lock_guard lock(m_ConfigMutex);
    return m_Config.interval;
}

void BackgroundSampler::setInterval(std::chrono::milliseconds newInterval)
{
    std::lock_guard lock(m_ConfigMutex);
    m_Config.interval = newInterval;
    spdlog::info("BackgroundSampler: interval changed to {}ms", newInterval.count());
}

void BackgroundSampler::samplerLoop(std::stop_token stopToken)
{
    spdlog::debug("BackgroundSampler: thread started");

    while (!stopToken.stop_requested())
    {
        auto startTime = std::chrono::steady_clock::now();

        // Enumerate processes
        auto counters = m_Probe->enumerate();
        const std::uint64_t totalCpuTime = m_Probe->totalCpuTime();

        // Invoke callback
        {
            std::lock_guard lock(m_CallbackMutex);
            if (m_Callback)
            {
                m_Callback(counters, totalCpuTime);
            }
        }

        // Clear refresh request
        m_RefreshRequested.store(false);

        // Get current interval
        std::chrono::milliseconds currentInterval;
        {
            std::lock_guard lock(m_ConfigMutex);
            currentInterval = m_Config.interval;
        }

        // Calculate sleep time
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        auto sleepTime = currentInterval - std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);

        // Sleep in small increments to check for stop/refresh requests
        constexpr auto checkInterval = std::chrono::milliseconds(50);
        while (sleepTime > std::chrono::milliseconds(0) && !stopToken.stop_requested() && !m_RefreshRequested.load())
        {
            auto sleepChunk = std::min(sleepTime, checkInterval);
            std::this_thread::sleep_for(sleepChunk);
            sleepTime -= sleepChunk;
        }
    }

    spdlog::debug("BackgroundSampler: thread exiting");
}

} // namespace Domain
