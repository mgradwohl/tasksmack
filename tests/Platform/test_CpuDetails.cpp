/// @file test_CpuDetails.cpp
/// @brief Tests for Platform::CpuTopology's pure core and cache arithmetic (#809), shared by the
/// Windows and Linux probes. No OS headers, so these run on every platform.

#include "Platform/CpuDetails.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace Platform
{
namespace
{

using CpuTopology::CacheInstance;
using CpuTopology::CoreRecord;

constexpr std::uint64_t KIB = 1024;
constexpr std::uint64_t MIB = 1024 * KIB;

TEST(CpuTopologyTest, SmtCoresCountTheirLogicalProcessors)
{
    // 8 cores with two hardware threads each, one efficiency class: not hybrid
    const std::vector<CoreRecord> cores(8, CoreRecord{.efficiencyClass = 0, .logicalProcessors = 2});
    CpuDetails details;
    CpuTopology::summarizeCores(cores, details);
    EXPECT_EQ(details.physicalCores, 8U);
    EXPECT_EQ(details.logicalProcessors, 16U);
    EXPECT_FALSE(details.performanceCores.has_value());
    EXPECT_FALSE(details.efficiencyCores.has_value());
}

TEST(CpuTopologyTest, HybridCoresSplitByTheHighestEfficiencyClass)
{
    // An Intel 13th-gen layout: 8 P-cores (class 1, SMT) and 16 E-cores (class 0, one thread each)
    std::vector<CoreRecord> cores(8, CoreRecord{.efficiencyClass = 1, .logicalProcessors = 2});
    cores.insert(cores.end(), 16, CoreRecord{.efficiencyClass = 0, .logicalProcessors = 1});
    CpuDetails details;
    CpuTopology::summarizeCores(cores, details);
    EXPECT_EQ(details.physicalCores, 24U);
    EXPECT_EQ(details.logicalProcessors, 32U);
    EXPECT_EQ(details.performanceCores, 8U);
    EXPECT_EQ(details.efficiencyCores, 16U);
}

TEST(CpuTopologyTest, NoCoreRecordsLeaveEverythingUnknown)
{
    CpuDetails details;
    CpuTopology::summarizeCores({}, details);
    EXPECT_EQ(details, CpuDetails{});
}

TEST(CpuTopologyTest, CachesSumEveryInstanceAtEachLevel)
{
    // 4 cores: 32 KiB L1d + 32 KiB L1i and 512 KiB L2 each, one shared 16 MiB L3
    std::vector<CacheInstance> caches;
    for (int core = 0; core < 4; ++core)
    {
        caches.push_back({.level = 1, .bytes = 32 * KIB});
        caches.push_back({.level = 1, .bytes = 32 * KIB});
        caches.push_back({.level = 2, .bytes = 512 * KIB});
    }
    caches.push_back({.level = 3, .bytes = 16 * MIB});
    CpuDetails details;
    CpuTopology::sumCacheInstances(caches, details);
    EXPECT_EQ(details.l1CacheBytes, 256 * KIB);
    EXPECT_EQ(details.l2CacheBytes, 2 * MIB);
    EXPECT_EQ(details.l3CacheBytes, 16 * MIB);
}

TEST(CpuTopologyTest, AMissingCacheLevelStaysUnknownNotZero)
{
    const std::vector<CacheInstance> caches{
        {.level = 1, .bytes = 64 * KIB}, {.level = 2, .bytes = 1 * MIB}, {.level = 4, .bytes = 128 * MIB}};
    CpuDetails details;
    CpuTopology::sumCacheInstances(caches, details);
    EXPECT_EQ(details.l1CacheBytes, 64 * KIB);
    EXPECT_EQ(details.l2CacheBytes, 1 * MIB);
    EXPECT_FALSE(details.l3CacheBytes.has_value()); // No L3; the L4 is not counted as one
}

TEST(CpuTopologyTest, DetailsAreReadAgainOnlyWhenTheProcessorCountChanges)
{
    EXPECT_FALSE(CpuTopology::cpuDetailsNeedRefresh(16, 16));
    EXPECT_TRUE(CpuTopology::cpuDetailsNeedRefresh(16, 20)); // Hot-added, or brought online
    EXPECT_TRUE(CpuTopology::cpuDetailsNeedRefresh(16, 12)); // Taken offline
    EXPECT_FALSE(CpuTopology::cpuDetailsNeedRefresh(16, 0)); // The per-core read failed: no count
    EXPECT_FALSE(CpuTopology::cpuDetailsNeedRefresh(0, 16)); // Not yet tied to a sample
}

TEST(CpuTopologyTest, TheSampledProcessorCountIsKeptUnlessThereIsNone)
{
    // A hot-add: re-read the details, and the probe's count (its cpuCoreCount) moves to the new one
    constexpr auto hotAdd = CpuTopology::updateProcessorCount(16, 20);
    EXPECT_TRUE(hotAdd.rereadDetails);
    EXPECT_EQ(hotAdd.processorCount, 20U);
    // Unchanged: nothing to re-read
    constexpr auto same = CpuTopology::updateProcessorCount(16, 16);
    EXPECT_FALSE(same.rereadDetails);
    EXPECT_EQ(same.processorCount, 16U);
    // A failed per-core read (0) keeps the count it had
    constexpr auto failed = CpuTopology::updateProcessorCount(16, 0);
    EXPECT_FALSE(failed.rereadDetails);
    EXPECT_EQ(failed.processorCount, 16U);
}

TEST(CpuTopologyTest, EfficiencyClassesAreKeptOnlyOnAHybridCpu)
{
    std::vector<std::uint8_t> classes;
    CpuTopology::setEfficiencyClass(classes, 0, 1);
    CpuTopology::setEfficiencyClass(classes, 2, 1); // Id 1 has no reading
    EXPECT_EQ(classes, (std::vector<std::uint8_t>{1, UNKNOWN_EFFICIENCY_CLASS, 1}));
    CpuTopology::keepOnlyIfHybrid(classes);
    EXPECT_TRUE(classes.empty()); // One class, an unread id aside: not hybrid

    CpuTopology::setEfficiencyClass(classes, 0, 1);
    CpuTopology::setEfficiencyClass(classes, 3, 0);
    CpuTopology::keepOnlyIfHybrid(classes);
    EXPECT_EQ(classes, (std::vector<std::uint8_t>{1, UNKNOWN_EFFICIENCY_CLASS, UNKNOWN_EFFICIENCY_CLASS, 0}));
}

} // namespace
} // namespace Platform
