/// @file test_WindowsAffinityMath.cpp
/// @brief Set affinity on Windows (#1881): splitting the chosen CPUs into processor-group masks, the
/// CPU set ids for several groups, and the dialog's layout from the cores. Pure, so built everywhere.

#include "Platform/CpuAffinity.h"
#include "Platform/CpuDetails.h"
#include "Platform/IProcessActions.h"
#include "Platform/Windows/WindowsAffinityMath.h"
#include "Platform/Windows/WindowsProcessProbeMath.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

TEST(GroupMasksForAffinityTest, OneGroupIsTheMaskItself)
{
    const std::vector<std::uint32_t> sizes{8};
    const auto masks = groupMasksForAffinity(CpuAffinity::fromMask(0b1001'0110U), sizes);
    ASSERT_TRUE(masks.has_value()) << masks.error();
    ASSERT_EQ(masks->size(), 1U);
    EXPECT_EQ(masks->front().group, 0U);
    EXPECT_EQ(masks->front().mask, 0b1001'0110U);
}

TEST(GroupMasksForAffinityTest, CpusSplitAcrossGroupsAndRoundTrip)
{
    // Two groups of 48 (a 96-thread server): CPU 47 is group 0's last, CPU 48 group 1's first.
    const std::vector<std::uint32_t> sizes{48, 48};
    CpuAffinity cpus;
    cpus.set(3);
    cpus.set(47);
    cpus.set(48);
    cpus.set(95);
    const auto masks = groupMasksForAffinity(cpus, sizes);
    ASSERT_TRUE(masks.has_value()) << masks.error();
    ASSERT_EQ(masks->size(), 2U);
    EXPECT_EQ((*masks)[0].group, 0U);
    EXPECT_EQ((*masks)[0].mask, (std::uint64_t{1} << 3U) | (std::uint64_t{1} << 47U));
    EXPECT_EQ((*masks)[1].group, 1U);
    EXPECT_EQ((*masks)[1].mask, (std::uint64_t{1} << 0U) | (std::uint64_t{1} << 47U));
    EXPECT_EQ(cpuAffinityFromGroupMasks(*masks, groupLayouts(sizes)), cpus) << "the probe reads back what was set";

    // Only group 1: group 0 gets no entry.
    CpuAffinity onlySecond;
    onlySecond.set(50);
    const auto second = groupMasksForAffinity(onlySecond, sizes);
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(second->size(), 1U);
    EXPECT_EQ(second->front().group, 1U);
    EXPECT_EQ(second->front().mask, std::uint64_t{1} << 2U);
}

TEST(GroupMasksForAffinityTest, RefusesNothingChosenUnknownGroupsAndMissingCpus)
{
    const std::vector<std::uint32_t> sizes{4};
    EXPECT_EQ(groupMasksForAffinity(CpuAffinity{}, sizes).error(), "Choose at least one CPU");
    EXPECT_FALSE(groupMasksForAffinity(CpuAffinity::fromMask(1U), {}).has_value());
    EXPECT_FALSE(groupMasksForAffinity(CpuAffinity::fromMask(0b1'0001U), sizes).has_value()) << "CPU 4 is past the only group";
}

TEST(CpuSetIdsForTest, MapsEachChosenProcessorToItsCpuSet)
{
    const std::vector<CpuSetRecord> sets{{.id = 256, .group = 0, .logicalProcessorIndex = 0},
                                         {.id = 257, .group = 0, .logicalProcessorIndex = 1},
                                         {.id = 300, .group = 1, .logicalProcessorIndex = 0}};
    const std::vector<GroupAffinityMask> masks{{.group = 0, .mask = 0b10U}, {.group = 1, .mask = 0b1U}};
    const auto ids = cpuSetIdsFor(masks, sets);
    ASSERT_TRUE(ids.has_value()) << ids.error();
    EXPECT_EQ(*ids, (std::vector<std::uint32_t>{257, 300}));

    const std::vector<GroupAffinityMask> missing{{.group = 1, .mask = 0b11U}};
    const auto refused = cpuSetIdsFor(missing, sets);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().find("1 of the 2"), std::string::npos) << refused.error();
}

TEST(AffinityLayoutFromCoresTest, NumbersCpusByGroupAndKeepsHybridClasses)
{
    // Group 0: a P-core (class 1) with CPUs 0-1 and an E-core (class 0) on CPU 2; group 1 (CPUs 4-7)
    // has an E-core on its processor 0, CPU 4. CPU 3 is a hot-add slot with no core yet.
    const std::vector<std::uint32_t> sizes{4, 4};
    const std::vector<ProcessorCoreRecord> cores{{.group = 0, .mask = 0b011U, .efficiencyClass = 1},
                                                 {.group = 0, .mask = 0b100U, .efficiencyClass = 0},
                                                 {.group = 1, .mask = 0b001U, .efficiencyClass = 0},
                                                 {.group = 7, .mask = 0b001U, .efficiencyClass = 0}}; // No such group: skipped
    const CpuAffinityLayout layout = affinityLayoutFromCores(sizes, cores);
    EXPECT_EQ(layout.groupSizes, sizes);
    EXPECT_TRUE(layout.appliedAsCpuSets);
    EXPECT_EQ(layout.online, CpuAffinity::fromMask(0b1'0111U));
    ASSERT_EQ(layout.efficiencyClassByCpu.size(), 5U);
    EXPECT_EQ(layout.efficiencyClassByCpu[0], 1U);
    EXPECT_EQ(layout.efficiencyClassByCpu[2], 0U);
    EXPECT_EQ(layout.efficiencyClassByCpu[3], UNKNOWN_EFFICIENCY_CLASS);
    EXPECT_EQ(layout.efficiencyClassByCpu[4], 0U);
}

TEST(AffinityLayoutFromCoresTest, OneKindOfCoreAndOneGroupCarryNoClassesOrCpuSets)
{
    const std::vector<std::uint32_t> sizes{4};
    const std::vector<ProcessorCoreRecord> cores{{.group = 0, .mask = 0b11U, .efficiencyClass = 0},
                                                 {.group = 0, .mask = 0b1100U, .efficiencyClass = 0}};
    const CpuAffinityLayout layout = affinityLayoutFromCores(sizes, cores);
    EXPECT_FALSE(layout.appliedAsCpuSets);
    EXPECT_TRUE(layout.efficiencyClassByCpu.empty());
    EXPECT_EQ(layout.online, CpuAffinity::fromMask(0b1111U));
}

} // namespace
} // namespace Platform
