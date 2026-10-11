#pragma once

#include "Domain/ISamplable.h"
#include "Domain/PublicationSlot.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SharedHistory.h"
#include "Platform/ISensorProbe.h"
#include "Platform/ISystemInfoProbe.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Domain
{

/// One sensor's value now and its extremes since TaskSmack started sampling it (#1522). NaN where there
/// is no reading: current after a failed read, min and max before the first good one.
struct SensorStats
{
    double current = 0.0;
    double min = 0.0;
    double max = 0.0;
};

/// One immutable generation of the sensor model (#1522). `sensors` is the layout the probe found, shared
/// by every generation; `stats` and `series` follow its readings device by device (Platform::sensorCount()
/// of them), and each series is aligned sample for sample with `timestamps` (NaN where a read failed).
/// The series are views of the model's shared history (#1412): a publish shares their samples, it
/// doesn't copy them.
struct SensorPublication
{
    std::uint64_t version = 0;
    std::shared_ptr<const Platform::SensorsInfo> sensors; ///< Null until the first sample has found them
    std::vector<SensorStats> stats;
    HistoryView<double> timestamps; ///< steady_clock seconds, as the other models' (UI::Widgets::historyFrameNowSeconds())
    std::vector<HistoryView<float>> series;
};

/// The live Sensors section's sampled model (#1522), as GPUModel is the GPU temperatures': the first
/// sample() finds the sensors (ISensorProbe::discover(), on the sampler thread), and every sample reads
/// their values, keeps each one's minimum and maximum since then, appends to a history bounded by the
/// history window, and publishes an immutable SensorPublication the UI adopts by pointer.
///
/// Thread-safe: sample() runs on a sampler thread while the UI thread calls setMaxHistorySeconds() and
/// publication(). Lock order: m_ProbeMutex -> m_Mutex -> the publication slot's (a leaf); the UI thread
/// takes only m_Mutex, so a slow probe read never holds it up.
class SensorModel : public ISamplable
{
  public:
    explicit SensorModel(std::unique_ptr<Platform::ISensorProbe> probe);
    ~SensorModel() override = default;

    SensorModel(const SensorModel&) = delete;
    SensorModel& operator=(const SensorModel&) = delete;
    SensorModel(SensorModel&&) = delete;
    SensorModel& operator=(SensorModel&&) = delete;

    void sample() override
    {
        sampleAt(std::chrono::steady_clock::now());
    }

    /// sample() at an explicit time, so the history window can be tested deterministically.
    void sampleAt(std::chrono::steady_clock::time_point now);

    /// The history window, clamped to SamplingConfig's range; trims and republishes at once once
    /// anything has been published, like the other models (#1145).
    void setMaxHistorySeconds(double seconds);

    [[nodiscard]] std::shared_ptr<const SensorPublication> publication() const noexcept
    {
        return m_Publication.load();
    }

    [[nodiscard]] std::uint64_t publicationVersion() const noexcept
    {
        return m_Publication.version();
    }

  private:
    /// Build the next generation from the state under m_Mutex and commit it. Caller holds m_Mutex.
    void publishLocked();
    /// Drop samples older than the window before @p nowSeconds, keeping the anchor sample as
    /// HistoryUtils::discardBefore() does (#1016). Caller holds m_Mutex.
    void trimLocked(double nowSeconds) noexcept;

    std::unique_ptr<Platform::ISensorProbe> m_Probe;
    std::mutex m_ProbeMutex;      // serialises sampleAt(): the probe, m_Values and discovery
    std::vector<double> m_Values; // the probe's latest read, sized at discovery; guarded by m_ProbeMutex
    bool m_Discovered = false;    // guarded by m_ProbeMutex

    std::mutex m_Mutex; // guards everything below
    std::shared_ptr<const Platform::SensorsInfo> m_Sensors;
    std::vector<SensorStats> m_Stats;
    SharedHistoryBuffer<double> m_Timestamps;
    std::vector<SharedHistoryBuffer<float>> m_Series;
    double m_MaxHistorySeconds = Sampling::HISTORY_SECONDS_DEFAULT;
    std::uint64_t m_Version = 0;

    PublicationSlot<SensorPublication> m_Publication;
};

} // namespace Domain
