/// @file test_SensorModel.cpp
/// @brief The live Sensors section's model (#1522): the first sample finds the sensors and later ones
/// only read them; each sensor's current, minimum and maximum since start; the history aligned with its
/// timestamps, NaN for a failed read, and bounded by the history window.

#include "Domain/SamplingConfig.h"
#include "Domain/SensorModel.h"
#include "Mocks/MockProbes.h"
#include "Platform/ISensorProbe.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace Domain
{
namespace
{

constexpr double NAN_VALUE = std::numeric_limits<double>::quiet_NaN();

/// A package temperature and a fan on one chip, read at 50 °C and 1000 RPM by discover().
[[nodiscard]] Platform::SensorsInfo twoSensors()
{
    Platform::SensorsInfo info;
    info.available = true;
    info.listed = true;
    info.devices.push_back({.name = "chip", .readings = {}});
    info.devices[0].readings.push_back(
        {.kind = Platform::SensorKind::Temperature, .label = "Package", .value = 50.0, .high = 90.0, .critical = {}});
    info.devices[0].readings.push_back({.kind = Platform::SensorKind::Fan, .label = "fan1", .value = 1000.0, .high = {}, .critical = {}});
    return info;
}

struct Fixture
{
    TestMocks::MockSensorProbe* probe = nullptr; // owned by the model
    std::unique_ptr<SensorModel> model;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    explicit Fixture(Platform::SensorsInfo sensors = twoSensors())
    {
        auto owned = std::make_unique<TestMocks::MockSensorProbe>(std::move(sensors));
        probe = owned.get();
        model = std::make_unique<SensorModel>(std::move(owned));
    }

    void sampleAfter(std::chrono::milliseconds offset, std::vector<double> values) const
    {
        probe->setValues(std::move(values));
        model->sampleAt(start + offset);
    }
};

TEST(SensorModelTest, PublishesNothingBeforeTheFirstSample)
{
    const Fixture fixture;
    const auto publication = fixture.model->publication();
    ASSERT_NE(publication, nullptr);
    EXPECT_EQ(publication->version, 0U);
    EXPECT_EQ(publication->sensors, nullptr);
    EXPECT_EQ(fixture.probe->discoveries.count(), 0); // found on the sampler thread, not at construction
}

TEST(SensorModelTest, TheFirstSampleFindsTheSensorsAndLaterOnesOnlyReadThem)
{
    const Fixture fixture;
    fixture.model->sampleAt(fixture.start);
    EXPECT_EQ(fixture.probe->discoveries.count(), 1);
    EXPECT_EQ(fixture.probe->reads.count(), 0); // discover()'s own read is the first sample

    const auto first = fixture.model->publication();
    ASSERT_NE(first->sensors, nullptr);
    EXPECT_EQ(Platform::sensorCount(*first->sensors), 2U);
    ASSERT_EQ(first->stats.size(), 2U);
    EXPECT_DOUBLE_EQ(first->stats[0].current, 50.0);
    EXPECT_DOUBLE_EQ(first->stats[1].current, 1000.0);

    fixture.sampleAfter(std::chrono::seconds(1), {51.0, 1010.0});
    fixture.sampleAfter(std::chrono::seconds(2), {52.0, 1020.0});
    EXPECT_EQ(fixture.probe->discoveries.count(), 1);
    EXPECT_EQ(fixture.probe->reads.count(), 2);
    const auto third = fixture.model->publication();
    EXPECT_EQ(third->version, 3U);
    EXPECT_EQ(third->sensors, first->sensors); // the layout is shared, not copied per sample
}

TEST(SensorModelTest, KeepsMinimumAndMaximumSinceStart)
{
    const Fixture fixture;
    fixture.model->sampleAt(fixture.start); // 50, 1000
    fixture.sampleAfter(std::chrono::seconds(1), {40.0, 1200.0});
    fixture.sampleAfter(std::chrono::seconds(2), {65.5, 900.0});
    fixture.sampleAfter(std::chrono::seconds(3), {55.0, NAN_VALUE}); // the fan's read failed

    const auto publication = fixture.model->publication();
    ASSERT_EQ(publication->stats.size(), 2U);
    const SensorStats& temperature = publication->stats[0];
    EXPECT_DOUBLE_EQ(temperature.current, 55.0);
    EXPECT_DOUBLE_EQ(temperature.min, 40.0);
    EXPECT_DOUBLE_EQ(temperature.max, 65.5);
    const SensorStats& fan = publication->stats[1];
    EXPECT_TRUE(std::isnan(fan.current)); // no reading now ...
    EXPECT_DOUBLE_EQ(fan.min, 900.0);     // ... but the extremes stay
    EXPECT_DOUBLE_EQ(fan.max, 1200.0);
}

TEST(SensorModelTest, ASensorNeverReadHasNoExtremes)
{
    Platform::SensorsInfo info = twoSensors();
    info.devices[0].readings[1].value = NAN_VALUE; // unreadable when found
    const Fixture fixture(std::move(info));
    fixture.model->sampleAt(fixture.start);
    fixture.sampleAfter(std::chrono::seconds(1), {50.0, NAN_VALUE});
    const auto publication = fixture.model->publication();
    EXPECT_TRUE(std::isnan(publication->stats[1].min));
    EXPECT_TRUE(std::isnan(publication->stats[1].max));
    fixture.sampleAfter(std::chrono::seconds(2), {50.0, 800.0});
    EXPECT_DOUBLE_EQ(fixture.model->publication()->stats[1].min, 800.0);
    EXPECT_DOUBLE_EQ(fixture.model->publication()->stats[1].max, 800.0);
}

TEST(SensorModelTest, HistoryIsAlignedWithItsTimestampsAndAFailedReadIsAGap)
{
    const Fixture fixture;
    fixture.model->sampleAt(fixture.start);
    fixture.sampleAfter(std::chrono::seconds(1), {51.0, NAN_VALUE});
    fixture.sampleAfter(std::chrono::seconds(2), {52.0, 1020.0});

    const auto publication = fixture.model->publication();
    ASSERT_EQ(publication->timestamps.size(), 3U);
    ASSERT_EQ(publication->series.size(), 2U);
    EXPECT_EQ(publication->series[0].size(), 3U);
    EXPECT_EQ(publication->series[1].size(), 3U);
    const double startSeconds = std::chrono::duration<double>(fixture.start.time_since_epoch()).count();
    EXPECT_DOUBLE_EQ(publication->timestamps[0], startSeconds);
    EXPECT_DOUBLE_EQ(publication->timestamps[2], startSeconds + 2.0);
    EXPECT_FLOAT_EQ(publication->series[0][1], 51.0F);
    EXPECT_TRUE(std::isnan(publication->series[1][1]));
    EXPECT_FLOAT_EQ(publication->series[1][2], 1020.0F);
}

TEST(SensorModelTest, HistoryIsBoundedByTheWindow)
{
    const Fixture fixture;
    fixture.model->setMaxHistorySeconds(Sampling::HISTORY_SECONDS_MIN); // 10 s
    fixture.model->sampleAt(fixture.start);
    int second = 1;
    while (second <= 30)
    {
        fixture.sampleAfter(std::chrono::seconds(second), {50.0 + second, 1000.0});
        ++second;
    }
    const auto publication = fixture.model->publication();
    // The window's samples (20..30 s) plus the one just before it, kept as the line's anchor (#1016).
    EXPECT_EQ(publication->timestamps.size(), 12U);
    EXPECT_EQ(publication->series[0].size(), publication->timestamps.size());
    EXPECT_FLOAT_EQ(publication->series[0].back(), 80.0F);
    // The extremes cover everything since start, not just the window.
    EXPECT_DOUBLE_EQ(publication->stats[0].min, 50.0);
    EXPECT_DOUBLE_EQ(publication->stats[0].max, 80.0);
}

TEST(SensorModelTest, ShorteningTheWindowTrimsAndRepublishesAtOnce)
{
    const Fixture fixture;
    fixture.model->sampleAt(fixture.start);
    int second = 1;
    while (second <= 60)
    {
        fixture.sampleAfter(std::chrono::seconds(second), {50.0, 1000.0});
        ++second;
    }
    const std::uint64_t before = fixture.model->publicationVersion();
    EXPECT_EQ(fixture.model->publication()->timestamps.size(), 61U);

    fixture.model->setMaxHistorySeconds(Sampling::HISTORY_SECONDS_MIN);
    EXPECT_EQ(fixture.model->publicationVersion(), before + 1);
    const auto publication = fixture.model->publication();
    EXPECT_EQ(publication->timestamps.size(), 12U);
    EXPECT_EQ(publication->series[1].size(), 12U);
}

TEST(SensorModelTest, ASampleNotLaterThanTheNewestUpdatesValuesButNotTheHistory)
{
    const Fixture fixture;
    fixture.model->sampleAt(fixture.start);
    fixture.sampleAfter(std::chrono::seconds(0), {70.0, 1000.0}); // the same instant again
    const auto publication = fixture.model->publication();
    EXPECT_EQ(publication->timestamps.size(), 1U);
    EXPECT_EQ(publication->series[0].size(), 1U);
    EXPECT_DOUBLE_EQ(publication->stats[0].current, 70.0);
    EXPECT_DOUBLE_EQ(publication->stats[0].max, 70.0);
}

TEST(SensorModelTest, NoSensorsPublishesAnEmptyLayout)
{
    SensorModel model(std::make_unique<Platform::UnsupportedSensorProbe>());
    model.sample();
    const auto publication = model.publication();
    EXPECT_EQ(publication->version, 1U);
    ASSERT_NE(publication->sensors, nullptr);
    EXPECT_TRUE(publication->sensors->devices.empty());
    EXPECT_TRUE(publication->stats.empty());
    EXPECT_TRUE(publication->series.empty());
    model.sample(); // and keeps sampling nothing, cheaply
    EXPECT_EQ(model.publicationVersion(), 2U);
}

} // namespace
} // namespace Domain
