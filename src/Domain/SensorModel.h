#pragma once

// The sensors as a metric (#2737): read on their own worker at the Settings interval, always, whether or
// not the System tab shows (the #2731 rule), with each sensor's range and history kept since start and
// published as an immutable SensorPublication the UI only reads.

#include "Domain/ISamplable.h"
#include "Domain/PublicationSlot.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SensorTracker.h"
#include "Platform/ISystemInfoProbe.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace Domain
{

/// One immutable read of the sensors, with each sensor's range and history so far (by sensorKey()).
struct SensorPublication
{
    std::uint64_t version = 0; ///< 0 until the first read.
    Platform::SensorsInfo sensors;
    std::map<std::string, SensorRange> ranges;
    std::map<std::string, SensorHistory> histories;
    /// The window the histories reach back (the Settings history duration, #2796): their charts' span.
    double historySeconds = Sampling::HISTORY_SECONDS_DEFAULT;
};

/// Reads the sensors from a BackgroundSampler of its own, so a slow hwmon or thermal-zone read delays
/// no other domain, and publishes each read as a SensorPublication.
class SensorModel : public ISamplable
{
  public:
    using Clock = std::chrono::steady_clock;
    using NowFunction = std::function<Clock::time_point()>;
    /// Reads the sensors once.
    using Reader = std::function<Platform::SensorsInfo()>;
    /// Makes the Reader: called by the first sample(), on the sampler's thread, so whatever probe it
    /// builds is built and first read there, not on the UI thread (#2731).
    using ReaderFactory = std::function<Reader()>;

    /// @param interval How often the sampler reads, which sizes the histories (setInterval() changes it).
    /// @param now Times each read (tests: a fake clock).
    explicit SensorModel(ReaderFactory makeReader, std::chrono::milliseconds interval, NowFunction now = [] { return Clock::now(); });

    /// Reads the sensors and publishes them with the ranges and histories. Thread-safe; samples are
    /// serialised.
    void sample() override;

    /// The sampler's new interval (the Settings refresh interval changed). Thread-safe.
    /// Clamped as BackgroundSampler clamps it.
    void setInterval(std::chrono::milliseconds interval) noexcept
    {
        m_IntervalMs.store(Sampling::clampRefreshInterval(interval.count()), std::memory_order_relaxed);
    }

    /// How far back each sensor's history reaches from the next sample on (the Settings history
    /// duration changed, #2796). Thread-safe. Clamped as the other models clamp it.
    void setHistorySeconds(double seconds) noexcept
    {
        m_HistorySeconds.store(Sampling::clampHistorySeconds(seconds), std::memory_order_relaxed);
    }

    /// The newest read (an empty version-0 one before the first sample).
    [[nodiscard]] std::shared_ptr<const SensorPublication> publication() const noexcept
    {
        return m_Slot.load();
    }

    [[nodiscard]] std::uint64_t version() const noexcept
    {
        return m_Slot.version();
    }

  private:
    ReaderFactory m_MakeReader;
    NowFunction m_Now;
    std::atomic<std::chrono::milliseconds::rep> m_IntervalMs;
    std::atomic<double> m_HistorySeconds{Sampling::HISTORY_SECONDS_DEFAULT};
    std::mutex m_SampleMutex;
    Reader m_Reader;                 // made by the first sample(); used under m_SampleMutex only
    bool m_ReaderAsked = false;      // the factory was called (once only, even if it made no reader)
    SensorTracker m_Tracker;         // guarded by m_SampleMutex
    std::uint64_t m_LastVersion = 0; // guarded by m_SampleMutex
    PublicationSlot<SensorPublication> m_Slot;
};

} // namespace Domain
