#include "SensorModel.h"

#include "Domain/SamplingConfig.h"
#include "Platform/ISystemInfoProbe.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace Domain
{

SensorModel::SensorModel(ReaderFactory makeReader, std::chrono::milliseconds interval, NowFunction now)
    : m_MakeReader(std::move(makeReader)), m_Now(std::move(now)), m_IntervalMs(Sampling::clampRefreshInterval(interval.count()))
{
    if (!m_MakeReader)
    {
        throw std::invalid_argument("SensorModel requires a reader factory");
    }
}

void SensorModel::sample()
{
    const std::scoped_lock lock(m_SampleMutex);
    if (!m_ReaderAsked)
    {
        m_ReaderAsked = true;
        m_Reader = m_MakeReader();
    }
    if (!m_Reader)
    {
        return; // nothing to read the sensors with, and making one isn't tried again
    }
    auto next = std::make_shared<SensorPublication>();
    next->sensors = m_Reader();
    const double nowSeconds = std::chrono::duration<double>(m_Now().time_since_epoch()).count();
    const double intervalSeconds = static_cast<double>(m_IntervalMs.load(std::memory_order_relaxed)) / 1000.0;
    next->historySeconds = m_HistorySeconds.load(std::memory_order_relaxed);
    m_Tracker.add(next->sensors, nowSeconds, intervalSeconds, next->historySeconds);
    next->ranges = m_Tracker.ranges();
    next->histories = m_Tracker.histories();
    next->version = ++m_LastVersion;
    m_Slot.commit(std::move(next));
}

} // namespace Domain
