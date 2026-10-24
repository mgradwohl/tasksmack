/// @file test_SensorModel.cpp
/// @brief The sensors as a metric (#2737): SensorModel builds its reader on the first sample (the
/// sampler's thread), keeps each sensor's range and history across samples, keeps the default window
/// of history and breaks it only on a real pause, whatever the interval; and a BackgroundSampler drives it at its interval with
/// nothing else asking.

#include "Domain/BackgroundSampler.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SensorModel.h"
#include "Domain/SensorTracker.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace Domain
{
namespace
{

using namespace std::chrono_literals;

/// One device with one temperature, at @p celsius.
Platform::SensorsInfo oneTemperature(double celsius)
{
    Platform::SensorsInfo info;
    info.available = true;
    info.listed = true;
    Platform::SensorDevice device;
    device.name = "coretemp";
    device.id = "hwmon0";
    device.readings.push_back(Platform::SensorReading{
        .kind = Platform::SensorKind::Temperature, .label = "Package", .id = "temp1", .value = celsius, .high = {}, .critical = {}});
    info.devices.push_back(std::move(device));
    return info;
}

std::string packageKey()
{
    const Platform::SensorsInfo info = oneTemperature(0.0);
    return sensorKey(info.devices.front(), info.devices.front().readings.front());
}

/// A clock the test moves by hand.
struct FakeClock
{
    std::shared_ptr<SensorModel::Clock::time_point> now = std::make_shared<SensorModel::Clock::time_point>(std::chrono::hours(1));

    [[nodiscard]] SensorModel::NowFunction function() const
    {
        return [now = now]
        {
            return *now;
        };
    }
    void advance(std::chrono::milliseconds by) const
    {
        *now += by;
    }
};

TEST(SensorModelTest, TheReaderIsMadeByTheFirstSampleNotTheConstructor)
{
    int made = 0;
    SensorModel model(
        [&made]
        {
            ++made;
            return SensorModel::Reader([] { return oneTemperature(40.0); });
        },
        1000ms);
    EXPECT_EQ(made, 0) << "the probe is built on the sampler's thread, not the UI's";
    EXPECT_EQ(model.version(), 0U);
    model.sample();
    model.sample();
    EXPECT_EQ(made, 1);
    EXPECT_EQ(model.version(), 2U);
}

TEST(SensorModelTest, EachSensorsRangeAndHistoryAreKeptAcrossSamples)
{
    const FakeClock clock;
    double celsius = 40.0;
    SensorModel model([&celsius] { return SensorModel::Reader([&celsius] { return oneTemperature(celsius); }); }, 1000ms, clock.function());
    for (const double reading : {40.0, 55.0, 47.0})
    {
        celsius = reading;
        model.sample();
        clock.advance(1000ms);
    }
    const std::shared_ptr<const SensorPublication> publication = model.publication();
    ASSERT_EQ(publication->version, 3U);
    const SensorRange& range = publication->ranges.at(packageKey());
    EXPECT_DOUBLE_EQ(range.min, 40.0);
    EXPECT_DOUBLE_EQ(range.max, 55.0);
    EXPECT_EQ(range.samples, 3U);
    const SensorHistory& history = publication->histories.at(packageKey());
    ASSERT_EQ(history.values.size(), 3U);
    EXPECT_FLOAT_EQ(history.values.back(), 47.0F);
    EXPECT_DOUBLE_EQ(history.times[1] - history.times[0], 1.0);
    EXPECT_DOUBLE_EQ(publication->sensors.devices.front().readings.front().value, 47.0);
}

TEST(SensorModelTest, TheHistoryHoldsTheDefaultWindowAtTheSamplingInterval)
{
    const FakeClock clock;
    SensorModel model([] { return SensorModel::Reader([] { return oneTemperature(40.0); }); }, 500ms, clock.function());
    const std::size_t window = static_cast<std::size_t>(Sampling::HISTORY_SECONDS_DEFAULT) * 2U; // readings at 500 ms
    for (std::size_t i = 0; i < window + 20; ++i)
    {
        model.sample();
        clock.advance(500ms);
    }
    // The window's worth, plus the reading at its very start.
    EXPECT_EQ(model.publication()->histories.at(packageKey()).values.size(), window + 1);
}

// #2796: the histories reach back the Settings history duration; the publication says which, for the charts.
TEST(SensorModelTest, TheHistoryFollowsTheSettingsHistoryDuration)
{
    const FakeClock clock;
    SensorModel model([] { return SensorModel::Reader([] { return oneTemperature(40.0); }); }, 1000ms, clock.function());
    model.setHistorySeconds(600.0);
    for (int i = 0; i < 700; ++i)
    {
        model.sample();
        clock.advance(1000ms);
    }
    EXPECT_DOUBLE_EQ(model.publication()->historySeconds, 600.0);
    EXPECT_EQ(model.publication()->histories.at(packageKey()).values.size(), 601U);

    // A shorter one trims what is kept at the next sample.
    model.setHistorySeconds(60.0);
    model.sample();
    EXPECT_DOUBLE_EQ(model.publication()->historySeconds, 60.0);
    EXPECT_EQ(model.publication()->histories.at(packageKey()).values.size(), 61U);

    // Clamped as the other models clamp it.
    model.setHistorySeconds(1.0);
    clock.advance(1000ms);
    model.sample();
    EXPECT_DOUBLE_EQ(model.publication()->historySeconds, static_cast<double>(Sampling::HISTORY_SECONDS_MIN));
}

TEST(SensorModelTest, ALongerIntervalKeepsTheHistoryAlreadyRead)
{
    const FakeClock clock;
    SensorModel model([] { return SensorModel::Reader([] { return oneTemperature(40.0); }); }, 100ms, clock.function());
    for (int i = 0; i < 100; ++i)
    {
        model.sample();
        clock.advance(100ms);
    }
    model.setInterval(5000ms);
    clock.advance(4900ms);
    model.sample();
    EXPECT_EQ(model.publication()->histories.at(packageKey()).values.size(), 101U) << "nothing dropped, no break";
}

TEST(SensorModelTest, AShorterIntervalOrASamplerBackingOffDoesntBreakTheLine)
{
    const FakeClock clock;
    SensorModel model([] { return SensorModel::Reader([] { return oneTemperature(40.0); }); }, 5000ms, clock.function());
    model.sample();
    model.setInterval(100ms);
    clock.advance(5000ms); // the first read at the new interval, a whole old interval later
    model.sample();
    clock.advance(800ms); // an overrun: the sampler waits out the pass
    model.sample();
    const SensorHistory& history = model.publication()->histories.at(packageKey());
    ASSERT_EQ(history.values.size(), 3U);
    for (const float value : history.values)
    {
        EXPECT_FALSE(std::isnan(value));
    }
}

TEST(SensorModelTest, ALongPauseBreaksTheLine)
{
    const FakeClock clock;
    SensorModel model([] { return SensorModel::Reader([] { return oneTemperature(40.0); }); }, 1000ms, clock.function());
    model.sample();
    clock.advance(std::chrono::milliseconds(static_cast<int>(sensorHistoryGapSeconds(1.0) * 1000.0) + 1000));
    model.sample();
    const SensorHistory& history = model.publication()->histories.at(packageKey());
    ASSERT_EQ(history.values.size(), 3U);
    EXPECT_TRUE(std::isnan(history.values[1]));
}

TEST(SensorModelTest, AFactoryThatMakesNoReaderPublishesNothingAndIsntAskedAgain)
{
    int asked = 0;
    SensorModel model(
        [&asked]
        {
            ++asked;
            return SensorModel::Reader{};
        },
        1000ms);
    model.sample();
    model.sample();
    EXPECT_EQ(model.version(), 0U);
    EXPECT_EQ(asked, 1);
}

// #2737's first test: with nothing drawing or asking, a sampler of its own reads the sensors at its
// interval; a longer interval reads them less often.
TEST(SensorModelTest, ASamplerOfItsOwnReadsTheSensorsAtItsIntervalWithNothingElseAsking)
{
    std::atomic<int> reads{0};
    auto model = std::make_shared<SensorModel>(
        [&reads]
        {
            return SensorModel::Reader(
                [&reads]
                {
                    ++reads;
                    return oneTemperature(40.0);
                });
        },
        100ms);
    BackgroundSampler sampler(SamplerConfig{.interval = 100ms, .firstSampleAfterInterval = false, .threadName = "ts-test-sensors"});
    sampler.addSamplable(model, "sensors");
    sampler.start();
    std::this_thread::sleep_for(1000ms);
    const int atShortInterval = reads.load();
    sampler.setInterval(5000ms);
    model->setInterval(5000ms);
    std::this_thread::sleep_for(150ms); // let the sample under way and the one due finish
    const int afterChange = reads.load();
    std::this_thread::sleep_for(1000ms);
    const int atLongInterval = reads.load() - afterChange;
    sampler.stop();

    // Loose bounds: a busy runner overruns and backs off, but not by a factor of three.
    EXPECT_GE(atShortInterval, 3) << "about 10 reads in 1 s at 100 ms";
    EXPECT_LE(atLongInterval, 1) << "at most one read in 1 s at 5 s";
}

} // namespace
} // namespace Domain
