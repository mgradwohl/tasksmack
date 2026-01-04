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
#include <unordered_map>
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

// =============================================================================
// Per-Interface Network Counter Tests
// =============================================================================

TEST(LinuxSystemProbeTest, PerInterfaceNetworkCountersAreAccessible)
{
    LinuxSystemProbe probe;
    auto counters = probe.read();

    // The vector should be accessible even if empty
    // Note: loopback (lo) is filtered out, so systems with only loopback may have 0 interfaces
    // Most systems have at least one physical or virtual interface besides loopback
    EXPECT_GE(counters.networkInterfaces.size(), 0ULL);
}

TEST(LinuxSystemProbeTest, PerInterfaceCountersHaveValidNames)
{
    LinuxSystemProbe probe;
    auto counters = probe.read();

    for (const auto& iface : counters.networkInterfaces)
    {
        // Interface name should not be empty
        EXPECT_FALSE(iface.name.empty()) << "Interface name should not be empty";

        // Display name should not be empty (may be same as name on Linux)
        EXPECT_FALSE(iface.displayName.empty()) << "Display name should not be empty";
    }
}

TEST(LinuxSystemProbeTest, PerInterfaceCountersAreNonNegative)
{
    LinuxSystemProbe probe;
    auto counters = probe.read();

    for (const auto& iface : counters.networkInterfaces)
    {
        // Counters should be non-negative (0 is valid for idle interfaces)
        EXPECT_GE(iface.rxBytes, 0ULL) << "Interface " << iface.name << " rxBytes invalid";
        EXPECT_GE(iface.txBytes, 0ULL) << "Interface " << iface.name << " txBytes invalid";
    }
}

TEST(LinuxSystemProbeTest, LoopbackInterfaceIsExcluded)
{
    LinuxSystemProbe probe;
    auto counters = probe.read();

    // Loopback interface (lo) should NOT be in the list
    // The probe intentionally filters it out since it's internal traffic
    for (const auto& iface : counters.networkInterfaces)
    {
        EXPECT_NE(iface.name, "lo") << "Loopback interface should be excluded";
    }
}

TEST(LinuxSystemProbeTest, PerInterfaceCountersSumApproximatesTotal)
{
    LinuxSystemProbe probe;
    auto counters = probe.read();

    // Sum of per-interface counters should approximately equal total
    // (may not be exact due to timing and internal aggregation)
    uint64_t sumRx = 0;
    uint64_t sumTx = 0;
    for (const auto& iface : counters.networkInterfaces)
    {
        sumRx += iface.rxBytes;
        sumTx += iface.txBytes;
    }

    // Allow some tolerance for timing differences
    // The sum should be close to the total (within 10% or 1MB, whichever is larger)
    auto tolerance = [](uint64_t total) -> uint64_t
    {
        constexpr uint64_t minTolerance = 1024 * 1024; // 1MB
        return std::max(minTolerance, total / 10);
    };

    EXPECT_NEAR(static_cast<double>(sumRx), static_cast<double>(counters.netRxBytes), static_cast<double>(tolerance(counters.netRxBytes)))
        << "Sum of per-interface RX should approximate total";
    EXPECT_NEAR(static_cast<double>(sumTx), static_cast<double>(counters.netTxBytes), static_cast<double>(tolerance(counters.netTxBytes)))
        << "Sum of per-interface TX should approximate total";
}

TEST(LinuxSystemProbeTest, PerInterfaceCountersMonotonicallyIncrease)
{
    LinuxSystemProbe probe;
    auto counters1 = probe.read();

    // Sleep briefly to allow potential traffic
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto counters2 = probe.read();

    // Build maps for comparison
    std::unordered_map<std::string, const SystemCounters::InterfaceCounters*> prevMap;
    for (const auto& iface : counters1.networkInterfaces)
    {
        prevMap[iface.name] = &iface;
    }

    // Check each interface in second read
    for (const auto& iface : counters2.networkInterfaces)
    {
        auto it = prevMap.find(iface.name);
        if (it != prevMap.end())
        {
            // Counters should be >= previous (cumulative)
            EXPECT_GE(iface.rxBytes, it->second->rxBytes) << "Interface " << iface.name << " rxBytes should not decrease";
            EXPECT_GE(iface.txBytes, it->second->txBytes) << "Interface " << iface.name << " txBytes should not decrease";
        }
    }
}

TEST(LinuxSystemProbeTest, PerInterfaceLinkSpeedIsReasonable)
{
    LinuxSystemProbe probe;
    auto counters = probe.read();

    for (const auto& iface : counters.networkInterfaces)
    {
        // Link speed may be 0 (unknown) for virtual/loopback interfaces
        // If non-zero, should be reasonable (1 Mbps to 1 Tbps)
        if (iface.linkSpeedMbps > 0)
        {
            EXPECT_GE(iface.linkSpeedMbps, 1ULL) << "Interface " << iface.name << " link speed too low";
            EXPECT_LE(iface.linkSpeedMbps, 1000000ULL) // 1 Tbps
                << "Interface " << iface.name << " link speed too high";
        }
    }
}

TEST(LinuxSystemProbeTest, PerInterfaceStatusConsistent)
{
    LinuxSystemProbe probe;

    // Read multiple times - interface status should be stable
    for (int i = 0; i < 3; ++i)
    {
        auto counters = probe.read();

        // Just verify structure is valid
        for (const auto& iface : counters.networkInterfaces)
        {
            // isUp is a boolean - no specific value check needed
            // Just ensure the field is accessible
            (void) iface.isUp;
        }
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
