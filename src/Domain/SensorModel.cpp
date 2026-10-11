#include "SensorModel.h"

#include "Domain/History.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SharedHistory.h"
#include "Platform/ISensorProbe.h"
#include "Platform/ISystemInfoProbe.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace Domain
{

namespace
{
constexpr double NO_READING = std::numeric_limits<double>::quiet_NaN();
} // namespace

SensorModel::SensorModel(std::unique_ptr<Platform::ISensorProbe> probe) : m_Probe(std::move(probe))
{
    m_Timestamps.setCapacity(Sampling::historyCapacityForSeconds(m_MaxHistorySeconds));
}

void SensorModel::sampleAt(std::chrono::steady_clock::time_point now)
{
    const std::scoped_lock probeLock(m_ProbeMutex);
    if (!m_Probe)
    {
        return;
    }
    std::shared_ptr<const Platform::SensorsInfo> found;
    if (!m_Discovered)
    {
        // Once, on the sampler thread: the slow listing. Its own read is the first sample.
        auto info = std::make_shared<Platform::SensorsInfo>(m_Probe->discover());
        m_Values.assign(Platform::sensorCount(*info), NO_READING);
        std::size_t index = 0;
        for (const Platform::SensorDevice& device : info->devices)
        {
            for (const Platform::SensorReading& reading : device.readings)
            {
                m_Values[index] = reading.value;
                ++index;
            }
        }
        m_Discovered = true;
        found = std::move(info);
    }
    else
    {
        m_Probe->readValues(m_Values);
    }

    const double nowSeconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    const std::scoped_lock lock(m_Mutex);
    const std::size_t count = m_Values.size();
    if (found)
    {
        const std::size_t capacity = Sampling::historyCapacityForSeconds(m_MaxHistorySeconds);
        std::vector<SharedHistoryBuffer<float>> series;
        series.reserve(count);
        while (series.size() < count)
        {
            series.emplace_back(capacity);
        }
        m_Stats.assign(count, SensorStats{.current = NO_READING, .min = NO_READING, .max = NO_READING});
        m_Series = std::move(series);
        m_Sensors = std::move(found);
    }

    // Every allocation first, so the appends below can't fail part way and leave the series misaligned.
    const bool append = m_Timestamps.empty() || nowSeconds > m_Timestamps.latest(); // in time order only
    if (append)
    {
        m_Timestamps.reserve(1);
        for (SharedHistoryBuffer<float>& series : m_Series)
        {
            series.reserve(1);
        }
        m_Timestamps.push(nowSeconds);
    }

    std::size_t index = 0;
    while (index < count)
    {
        const double value = m_Values[index];
        SensorStats& stats = m_Stats[index];
        stats.current = value;
        if (!std::isnan(value))
        {
            stats.min = (std::isnan(stats.min) || value < stats.min) ? value : stats.min;
            stats.max = (std::isnan(stats.max) || value > stats.max) ? value : stats.max;
        }
        if (append)
        {
            m_Series[index].push(static_cast<float>(value));
        }
        ++index;
    }
    trimLocked(nowSeconds);
    publishLocked();
}

void SensorModel::setMaxHistorySeconds(double seconds)
{
    const std::scoped_lock lock(m_Mutex);
    m_MaxHistorySeconds = Sampling::clampHistorySeconds(seconds);
    const std::size_t capacity = Sampling::historyCapacityForSeconds(m_MaxHistorySeconds);
    m_Timestamps.setCapacity(capacity);
    for (SharedHistoryBuffer<float>& series : m_Series)
    {
        series.setCapacity(capacity);
    }
    if (m_Version == 0)
    {
        return; // nothing published yet
    }
    if (!m_Timestamps.empty())
    {
        trimLocked(m_Timestamps.latest());
    }
    publishLocked();
}

void SensorModel::trimLocked(double nowSeconds) noexcept
{
    const std::size_t removed = HistoryUtils::discardBefore(m_Timestamps, nowSeconds - m_MaxHistorySeconds);
    for (SharedHistoryBuffer<float>& series : m_Series)
    {
        series.discardFront(removed);
    }
}

void SensorModel::publishLocked()
{
    auto next = std::make_shared<SensorPublication>();
    next->version = m_Version + 1;
    next->sensors = m_Sensors;
    next->stats = m_Stats;
    next->timestamps = m_Timestamps.view();
    next->series.reserve(m_Series.size());
    for (const SharedHistoryBuffer<float>& series : m_Series)
    {
        next->series.push_back(series.view());
    }
    m_Version = next->version;
    m_Publication.commit(std::move(next));
}

} // namespace Domain
