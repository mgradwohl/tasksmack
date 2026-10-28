/// @file test_StartupImpact.cpp
/// @brief The Startup impact rating (#3073): Task Manager's thresholds at and around each boundary, and
/// an entry's impact from a sign-in's measurement (None when disabled, Not measured when its program
/// didn't run or is unknown).

#include "Domain/StartupImpact.h"
#include "Platform/IStartupProbe.h"

#include <gtest/gtest.h>

namespace Domain
{
namespace
{

TEST(StartupImpactTest, LowIsUnderBothLowThresholds)
{
    EXPECT_EQ(rateStartupImpact(0, 0), StartupImpact::Low);
    EXPECT_EQ(rateStartupImpact(299'999, 299'999), StartupImpact::Low);
    EXPECT_EQ(rateStartupImpact(300'000, 0), StartupImpact::Medium) << "300 ms is not under 300 ms";
    EXPECT_EQ(rateStartupImpact(0, 300'000), StartupImpact::Medium) << "300 KB is not under 300 KB";
}

TEST(StartupImpactTest, HighIsOverEitherHighThreshold)
{
    EXPECT_EQ(rateStartupImpact(1'000'000, 3'000'000), StartupImpact::Medium) << "1 s and 3 MB exactly are not over";
    EXPECT_EQ(rateStartupImpact(1'000'001, 0), StartupImpact::High);
    EXPECT_EQ(rateStartupImpact(0, 3'000'001), StartupImpact::High);
    EXPECT_EQ(rateStartupImpact(13'239'718, 100'827'136), StartupImpact::High);
    EXPECT_EQ(rateStartupImpact(412'000, 2'097'152), StartupImpact::Medium);
    EXPECT_EQ(rateStartupImpact(26'041, 204'800), StartupImpact::Low);
}

TEST(StartupImpactTest, TheRatingsOrderFromNoneToHigh)
{
    EXPECT_LT(StartupImpact::None, StartupImpact::NotMeasured);
    EXPECT_LT(StartupImpact::NotMeasured, StartupImpact::Low);
    EXPECT_LT(StartupImpact::Low, StartupImpact::Medium);
    EXPECT_LT(StartupImpact::Medium, StartupImpact::High);
}

[[nodiscard]] Platform::StartupImpactReading measuredSignIn()
{
    Platform::StartupImpactReading measured;
    measured.processes = {
        {.path = R"(C:\Apps\Busy.exe)",
         .pid = 10,
         .parentPid = 1,
         .startTicks = 5,
         .parentStartTicks = 1,
         .cpuUs = 1'500'000,
         .diskBytes = 10},
        {.path = R"(C:\Apps\Child.exe)",
         .pid = 11,
         .parentPid = 10,
         .startTicks = 6,
         .parentStartTicks = 5,
         .cpuUs = 100,
         .diskBytes = 4'000'000},
        {.path = R"(C:\Apps\Quiet.exe)",
         .pid = 12,
         .parentPid = 1,
         .startTicks = 7,
         .parentStartTicks = 1,
         .cpuUs = 1000,
         .diskBytes = 1000},
    };
    measured.signInUnixSeconds = 1792774334;
    return measured;
}

[[nodiscard]] Platform::StartupEntry startupProgram(const char* path, bool enabled = true)
{
    Platform::StartupEntry startup;
    startup.executablePath = path;
    startup.enabled = enabled;
    return startup;
}

TEST(StartupImpactTest, AnEntrysImpactComesFromItsProgramsProcesses)
{
    const StartupEntryImpact busy = startupEntryImpact(startupProgram(R"(C:\Apps\Busy.exe)"), measuredSignIn());
    EXPECT_EQ(busy.rating, StartupImpact::High);
    EXPECT_EQ(busy.cpuUs, 1'500'100U);
    EXPECT_EQ(busy.diskBytes, 4'000'010U);
    EXPECT_EQ(busy.processCount, 2U);

    const StartupEntryImpact quiet = startupEntryImpact(startupProgram(R"(c:\apps\quiet.EXE)"), measuredSignIn());
    EXPECT_EQ(quiet.rating, StartupImpact::Low);
    EXPECT_EQ(quiet.processCount, 1U);
}

TEST(StartupImpactTest, DisabledIsNoneAndUnmeasuredIsNotMeasured)
{
    const StartupEntryImpact disabled = startupEntryImpact(startupProgram(R"(C:\Apps\Busy.exe)", false), measuredSignIn());
    EXPECT_EQ(disabled.rating, StartupImpact::None);
    EXPECT_EQ(disabled.processCount, 0U) << "what it cost before doesn't count once it is off";

    const StartupEntryImpact absent = startupEntryImpact(startupProgram(R"(C:\Apps\Absent.exe)"), measuredSignIn());
    EXPECT_EQ(absent.rating, StartupImpact::NotMeasured);
    EXPECT_EQ(absent.cpuUs, 0U);

    EXPECT_EQ(startupEntryImpact(startupProgram(""), measuredSignIn()).rating, StartupImpact::NotMeasured) << "program unknown";
    EXPECT_EQ(startupEntryImpact(startupProgram(R"(C:\Apps\Busy.exe)"), {}).rating, StartupImpact::NotMeasured) << "no measurement";
}

} // namespace
} // namespace Domain
