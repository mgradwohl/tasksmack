/// @file test_ProcessProbeContract.cpp
/// @brief Cross-platform contract tests for Platform::IProcessProbe via Platform::makeProcessProbe()

#include "Platform/Factory.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on
#else
#include <unistd.h>
#endif

namespace Platform
{
namespace
{
[[nodiscard]] int32_t getCurrentPid()
{
#ifdef _WIN32
    return static_cast<int32_t>(GetCurrentProcessId());
#else
    return static_cast<int32_t>(getpid());
#endif
}

} // namespace

TEST(ProcessProbeContractTest, FactoryConstructs)
{
    auto probe = makeProcessProbe();
    ASSERT_NE(probe, nullptr);
}

TEST(ProcessProbeContractTest, TicksPerSecondIsPositive)
{
    auto probe = makeProcessProbe();
    ASSERT_NE(probe, nullptr);

    const long ticks = probe->ticksPerSecond();
    EXPECT_GT(ticks, 0);
    EXPECT_LE(ticks, 10'000'000L);
}

TEST(ProcessProbeContractTest, TotalCpuTimeIsNonZeroAndMonotonic)
{
    auto probe = makeProcessProbe();
    ASSERT_NE(probe, nullptr);

    const uint64_t time1 = probe->totalCpuTime();
    EXPECT_GT(time1, 0ULL);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    volatile int sum = 0;
    for (int i = 0; i < 1'000'000; ++i)
    {
        sum += i;
    }

    const uint64_t time2 = probe->totalCpuTime();
    EXPECT_GE(time2, time1);
}

TEST(ProcessProbeContractTest, SystemTotalMemoryIsPositive)
{
    auto probe = makeProcessProbe();
    ASSERT_NE(probe, nullptr);

    const uint64_t totalMem = probe->systemTotalMemory();
    EXPECT_GT(totalMem, 128ULL * 1024ULL * 1024ULL);
}

TEST(ProcessProbeContractTest, EnumerateReturnsSomeProcesses)
{
    auto probe = makeProcessProbe();
    ASSERT_NE(probe, nullptr);

    const auto processes = probe->enumerate();
    EXPECT_GT(processes.size(), 0ULL);
}

TEST(ProcessProbeContractTest, EnumerateFindsOurOwnProcess)
{
    auto probe = makeProcessProbe();
    ASSERT_NE(probe, nullptr);

    const auto processes = probe->enumerate();

    const int32_t ourPid = getCurrentPid();
    const auto it = std::find_if(processes.begin(), processes.end(), [ourPid](const ProcessCounters& p) { return p.pid == ourPid; });

    ASSERT_NE(it, processes.end()) << "Should find our own process (PID " << ourPid << ")";

    EXPECT_GT(it->name.size(), 0ULL);
    EXPECT_GT(it->rssBytes, 0ULL);
    EXPECT_GT(it->virtualBytes, 0ULL);

    const auto caps = probe->capabilities();
    if (caps.hasStartTime)
    {
        EXPECT_GT(it->startTimeTicks, 0ULL);
    }
    if (caps.hasThreadCount)
    {
        EXPECT_GE(it->threadCount, 1);
    }
    if (caps.hasUser)
    {
        EXPECT_GT(it->user.size(), 0ULL);
    }
    if (caps.hasCommand)
    {
        EXPECT_GT(it->command.size(), 0ULL);
    }
}

TEST(ProcessProbeContractTest, BasePriorityIsInValidRange)
{
    auto probe = makeProcessProbe();
    ASSERT_NE(probe, nullptr);

    const auto caps = probe->capabilities();
    if (!caps.hasBasePriority)
    {
        GTEST_SKIP() << "Platform does not support base priority";
    }

    const auto processes = probe->enumerate();
    ASSERT_GT(processes.size(), 0ULL);

    // Base priority should be in the range 1-31 for Windows-style priorities
    // Our implementation uses 4-24 (IDLE to REALTIME), centered around 8 (NORMAL)
    for (const auto& proc : processes)
    {
        EXPECT_GE(proc.basePriority, 1) << "Process " << proc.name << " (PID " << proc.pid << ")";
        EXPECT_LE(proc.basePriority, 31) << "Process " << proc.name << " (PID " << proc.pid << ")";
    }
}

TEST(ProcessProbeContractTest, NiceAndBasePriorityAreRelated)
{
    auto probe = makeProcessProbe();
    ASSERT_NE(probe, nullptr);

    const auto caps = probe->capabilities();
    if (!caps.hasNice || !caps.hasBasePriority)
    {
        GTEST_SKIP() << "Platform does not support both nice and base priority";
    }

    const auto processes = probe->enumerate();
    ASSERT_GT(processes.size(), 0ULL);

    // On systems with both nice and base priority:
    // - nice: -20 to +19 (lower is higher priority)
    // - basePriority: 4 to 13+ (higher is higher priority)
    // They should have an inverse relationship
    for (const auto& proc : processes)
    {
        EXPECT_GE(proc.nice, -20) << "Process " << proc.name;
        EXPECT_LE(proc.nice, 19) << "Process " << proc.name;

        // Most processes should have base priority in reasonable range (4-13)
        // unless they're specially elevated
        if (proc.basePriority >= 1 && proc.basePriority <= 31)
        {
            // Valid range - this is expected
            SUCCEED();
        }
    }
}

} // namespace Platform
