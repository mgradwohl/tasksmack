// SensorModel's sample under allocation failure (#3342 step 4): a std::bad_alloc anywhere in a sample -- one
// where a device appears, the reads paused (so each history gets its break as well as the reading) and the
// history window shrank -- must leave every sensor's values aligned with its times, publish nothing half
// built, and let the next sample apply cleanly.
//
// The failure is real, not simulated: AllocationFailureHook makes the Nth allocation the current thread makes
// from now throw std::bad_alloc. SensorModel::sample() lets it escape (BackgroundSampler catches and logs it),
// so the test catches it and asks the hook whether it fired.

#include "AllocationFailureHook.h"
#include "Domain/SensorModel.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>

using TestSupport::FailAllocationAfter;

namespace
{

using namespace std::chrono_literals;

constexpr int WARM_UP_SAMPLES = 30;

Platform::SensorReading temperature(const char* label, const char* id, double celsius)
{
    return Platform::SensorReading{
        .kind = Platform::SensorKind::Temperature, .label = label, .id = id, .value = celsius, .high = {}, .critical = {}};
}

/// coretemp with two readings, and, when @p withNvme, a second device that appears later.
Platform::SensorsInfo sensors(double celsius, bool withNvme)
{
    Platform::SensorsInfo info;
    info.available = true;
    info.listed = true;
    Platform::SensorDevice cpu;
    cpu.name = "coretemp";
    cpu.id = "hwmon0";
    cpu.readings.push_back(temperature("Package", "temp1", celsius));
    cpu.readings.push_back(temperature("Core 0", "temp2", celsius - 5.0));
    info.devices.push_back(std::move(cpu));
    if (withNvme)
    {
        Platform::SensorDevice nvme;
        nvme.name = "nvme";
        nvme.id = "hwmon1";
        nvme.readings.push_back(temperature("Composite", "temp1", 38.0));
        info.devices.push_back(std::move(nvme));
    }
    return info;
}

/// Every sensor's published values are as many as its times, and its times only move forward.
void expectAligned(const Domain::SensorPublication& publication)
{
    for (const auto& [key, history] : publication.histories)
    {
        ASSERT_EQ(history.values.size(), history.times.size()) << key;
        for (std::size_t i = 1; i < history.times.size(); ++i)
        {
            EXPECT_LT(history.times[i - 1], history.times[i]) << key << " at " << i;
        }
    }
}

// Fails each allocation of one sample in turn until the sample needs no more. After every failure the
// publication is the last whole one (or the whole new one), every series is aligned, and the next sample adds
// the new device and its reading to aligned histories.
TEST(SensorModelAllocationFailureTest, AFailedAllocationOnASampleThatAddsADeviceAfterAPauseKeepsTheHistoriesAligned)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    constexpr std::int64_t MAX_ALLOCATIONS = 100'000; // a bound on the loop, far above one sample's

    std::int64_t failed = 0;
    for (std::int64_t failAt = 0; failAt < MAX_ALLOCATIONS; ++failAt)
    {
        SCOPED_TRACE("failing allocation " + std::to_string(failAt));
        auto now = std::make_shared<Domain::SensorModel::Clock::time_point>(std::chrono::hours(1));
        auto celsius = std::make_shared<double>(40.0);
        auto withNvme = std::make_shared<bool>(false);
        Domain::SensorModel model([celsius, withNvme]
                                  { return Domain::SensorModel::Reader([celsius, withNvme] { return sensors(*celsius, *withNvme); }); },
                                  1000ms,
                                  [now] { return *now; });
        for (int step = 0; step < WARM_UP_SAMPLES; ++step)
        {
            *celsius = 40.0 + static_cast<double>(step % 7);
            model.sample();
            *now += 1000ms;
        }
        const auto before = model.publication();
        ASSERT_EQ(before->version, static_cast<std::uint64_t>(WARM_UP_SAMPLES));

        // The failing sample: a new device, after a pause (each history gets its break too), into a window
        // that shrank below what the histories hold.
        *withNvme = true;
        *celsius = 60.0;
        *now += 10s;
        model.setHistorySeconds(10.0);
        bool fired = false;
        {
            const FailAllocationAfter guard(failAt);
            try
            {
                model.sample();
            }
            catch (const std::bad_alloc&)
            {
                // the failure this test injects; the hook says whether it fired
            }
            fired = !TestSupport::allocationFailurePending();
        }
        if (!fired)
        {
            break; // the sample made fewer than failAt + 1 allocations: every one has been failed
        }
        ++failed;

        // Nothing half built is published: the last whole publication, or the whole new one.
        const auto afterFailure = model.publication();
        if (afterFailure->version == before->version)
        {
            EXPECT_EQ(afterFailure, before);
        }
        else
        {
            EXPECT_EQ(afterFailure->version, before->version + 1);
        }
        expectAligned(*afterFailure);

        // The model carries on: the next sample publishes every sensor, the new device included, aligned.
        *now += 1000ms;
        *celsius = 61.0;
        model.sample();
        const auto next = model.publication();
        EXPECT_GT(next->version, before->version);
        expectAligned(*next);
        EXPECT_EQ(next->histories.size(), 3U);
        for (const auto& [key, history] : next->histories)
        {
            ASSERT_FALSE(history.values.empty()) << key;
            EXPECT_DOUBLE_EQ(history.times[history.times.size() - 1], std::chrono::duration<double>(now->time_since_epoch()).count())
                << key;
        }
        if (HasFailure())
        {
            return; // one failing allocation point is enough to report
        }
    }
    EXPECT_GT(failed, 5); // the sample did allocate, and those allocations were exercised
}

} // namespace
