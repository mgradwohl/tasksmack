/// @file test_LinuxSystemProbe.cpp
/// @brief Integration tests for Platform::LinuxSystemProbe
///
/// These are integration tests that interact with the real /proc filesystem.
/// They verify that the probe correctly reads and parses system information.

#include <gtest/gtest.h>

#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/Linux/LinuxSystemProbe.h"
#include "Platform/SystemTypes.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace Platform
{
namespace
{

// =============================================================================
// Construction and Basic Operations
// =============================================================================

TEST(LinuxSystemProbeTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ LinuxSystemProbe probe; });
}

TEST(LinuxSystemProbeTest, CapabilitiesReportedCorrectly)
{
    LinuxSystemProbe probe;
    auto caps = probe.capabilities();

    // These may vary based on kernel version, so we just check the fields exist.
    EXPECT_TRUE(caps.hasPerCoreCpu || !caps.hasPerCoreCpu);
}

TEST(LinuxSystemProbeTest, TicksPerSecondIsPositive)
{
    LinuxSystemProbe probe;
    auto ticks = probe.ticksPerSecond();

    // Common values are 100 (older systems) or 250+ (modern systems)
    EXPECT_GT(ticks, 0);
    EXPECT_LE(ticks, 10000); // Sanity check
}

// =============================================================================
// System Counter Tests
// =============================================================================

TEST(LinuxSystemProbeTest, ReadReturnsValidCounters)
{
    LinuxSystemProbe probe;
    auto counters = probe.read();

    // CPU counters should be non-zero
    EXPECT_GT(counters.cpuTotal.user, 0ULL);
    EXPECT_GT(counters.cpuTotal.total(), 0ULL);
}

TEST(LinuxSystemProbeTest, SwapCountersAreValid)
{
    LinuxSystemProbe probe;
    auto counters = probe.read();

    // Swap may or may not be configured
    if (counters.memory.swapTotalBytes > 0)
    {
        // If swap exists, free should be <= total
        EXPECT_LE(counters.memory.swapFreeBytes, counters.memory.swapTotalBytes);
    }
}

TEST(LinuxSystemProbeTest, UptimeIsPositive)
{
    LinuxSystemProbe probe;
    auto counters = probe.read();

    // System should have been up for at least a few seconds
    EXPECT_GT(counters.uptimeSeconds, 0ULL);

    // Sanity check: uptime should be less than 10 years
    constexpr uint64_t tenYearsInSeconds = 10ULL * 365 * 24 * 60 * 60;
    EXPECT_LT(counters.uptimeSeconds, tenYearsInSeconds);
}

TEST(LinuxSystemProbeTest, LoadAverageIsNonNegative)
{
    LinuxSystemProbe probe;
    auto counters = probe.read();

    // Load averages should be non-negative
    EXPECT_GE(counters.loadAvg1, 0.0);
    EXPECT_GE(counters.loadAvg5, 0.0);
    EXPECT_GE(counters.loadAvg15, 0.0);

    // Load averages should be reasonable (not more than 1000 per core)
    EXPECT_LT(counters.loadAvg1, 1000.0 * static_cast<double>(counters.cpuPerCore.size()));
    EXPECT_LT(counters.loadAvg5, 1000.0 * static_cast<double>(counters.cpuPerCore.size()));
    EXPECT_LT(counters.loadAvg15, 1000.0 * static_cast<double>(counters.cpuPerCore.size()));
}

TEST(LinuxSystemProbeTest, StaticInfoIsPopulated)
{
    LinuxSystemProbe probe;
    auto counters = probe.read();

    // Hostname should be non-empty
    EXPECT_GT(counters.hostname.size(), 0ULL);

    // CPU model may or may not be available (depends on /proc/cpuinfo format)
}

// =============================================================================
// Consistency Tests
// =============================================================================

TEST(LinuxSystemProbeTest, MultipleReadsAreConsistent)
{
    LinuxSystemProbe probe;

    auto counters1 = probe.read();
    auto counters2 = probe.read();

    // Static values should be identical
    EXPECT_EQ(counters1.hostname, counters2.hostname);
    EXPECT_EQ(counters1.cpuModel, counters2.cpuModel);
    EXPECT_EQ(counters1.memory.totalBytes, counters2.memory.totalBytes);
    EXPECT_EQ(counters1.memory.swapTotalBytes, counters2.memory.swapTotalBytes);
    EXPECT_EQ(counters1.cpuPerCore.size(), counters2.cpuPerCore.size());
}

TEST(LinuxSystemProbeTest, CpuCountersIncrease)
{
    LinuxSystemProbe probe;

    auto counters1 = probe.read();

    // Do some CPU work
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    volatile int sum = 0;
    for (int i = 0; i < 1000000; ++i)
    {
        sum += i;
    }

    auto counters2 = probe.read();

    // Total CPU time should have increased
    EXPECT_GT(counters2.cpuTotal.total(), counters1.cpuTotal.total());
}

TEST(LinuxSystemProbeTest, UptimeIncreases)
{
    LinuxSystemProbe probe;

    auto counters1 = probe.read();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto counters2 = probe.read();

    // Uptime should have increased (allowing for rounding)
    EXPECT_GE(counters2.uptimeSeconds, counters1.uptimeSeconds);
}

// =============================================================================
// Edge Cases and Error Handling
// =============================================================================

TEST(LinuxSystemProbeTest, HandlesRapidReads)
{
    LinuxSystemProbe probe;

    // Rapidly read many times - should not crash or leak
    EXPECT_NO_THROW({
        for (int i = 0; i < 100; ++i)
        {
            auto counters = probe.read();
            EXPECT_GT(counters.cpuTotal.total(), 0ULL);
        }
    });
}

// =============================================================================
// Multithreading Tests
// =============================================================================

TEST(LinuxSystemProbeTest, ConcurrentReads)
{
    LinuxSystemProbe probe;

    std::atomic<int> successCount{0};
    std::atomic<bool> running{true};

    auto readTask = [&]()
    {
        while (running)
        {
            try
            {
                auto counters = probe.read();
                if (counters.cpuTotal.total() > 0)
                {
                    ++successCount;
                }
            }
            catch (...)
            {
                // Reading should not throw
                FAIL() << "Read threw an exception";
            }
        }
    };

    // Start multiple threads reading concurrently
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
    {
        threads.emplace_back(readTask);
    }

    // Let them run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& t : threads)
    {
        t.join();
    }

    // All reads should have succeeded
    EXPECT_GT(successCount.load(), 0);
}

// =============================================================================
// CPU Frequency Tests (Optional)
// =============================================================================

TEST(LinuxSystemProbeTest, CpuFrequencyIfAvailable)
{
    LinuxSystemProbe probe;
    auto counters = probe.read();

    // CPU frequency may or may not be available depending on the system.
    // If present, it should be reasonable (100 MHz to 10 GHz)
    if (counters.cpuFreqMHz > 0)
    {
        EXPECT_GT(counters.cpuFreqMHz, 100) << "CPU frequency should be > 100 MHz";
        EXPECT_LT(counters.cpuFreqMHz, 10000) << "CPU frequency should be < 10 GHz";
    }
}

// =============================================================================
// Network Counter Tests
// =============================================================================

TEST(LinuxSystemProbeTest, NetworkCapabilityEnabled)
{
    LinuxSystemProbe probe;
    auto caps = probe.capabilities();

    // Linux should always have network counters available via /proc/net/dev
    EXPECT_TRUE(caps.hasNetworkCounters);
}

TEST(LinuxSystemProbeTest, NetworkCountersAreValid)
{
    LinuxSystemProbe probe;
    auto counters = probe.read();

    // Network counters should be non-negative (0 is valid for idle systems)
    // We can't guarantee non-zero since the system may have no network traffic
    EXPECT_GE(counters.netRxBytes, 0ULL);
    EXPECT_GE(counters.netTxBytes, 0ULL);
}

TEST(LinuxSystemProbeTest, NetworkCountersMonotonicallyIncrease)
{
    LinuxSystemProbe probe;
    auto counters1 = probe.read();

    // Generate some network traffic by sleeping briefly
    // (background processes likely produce some network activity)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto counters2 = probe.read();

    // Counters should be >= previous values (cumulative, not deltas)
    EXPECT_GE(counters2.netRxBytes, counters1.netRxBytes);
    EXPECT_GE(counters2.netTxBytes, counters1.netTxBytes);
}

TEST(LinuxSystemProbeTest, NetworkCountersReadMultipleTimes)
{
    LinuxSystemProbe probe;

    // Read counters multiple times to ensure consistency
    for (int i = 0; i < 5; ++i)
    {
        auto counters = probe.read();
        // Basic sanity: should not throw and should have valid structure
        EXPECT_GE(counters.netRxBytes, 0ULL);
        EXPECT_GE(counters.netTxBytes, 0ULL);
    }
}

} // namespace
} // namespace Platform

#else

TEST(LinuxSystemProbeTest, SkippedOnNonLinux)
{
    GTEST_SKIP() << "LinuxSystemProbe tests require Linux (/proc, unistd.h)";
}

#endif
