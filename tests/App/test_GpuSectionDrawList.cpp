/// @file test_GpuSectionDrawList.cpp
/// @brief Tests for GpuSection::gpuDrawList(), which decides which GPUs the GPU tab draws, in what
/// order, and under which IDs (#1163).

#include "App/Panels/GpuSection.h"
#include "Domain/GPUModel.h"
#include "Domain/GPUSnapshot.h"
#include "Platform/GPUTypes.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace App
{
namespace
{

using GpuSection::GpuDrawEntry;
using GpuSection::gpuDrawList;

Platform::GPUInfo info(const std::string& id)
{
    Platform::GPUInfo gpu;
    gpu.id = id;
    gpu.name = "Name " + id;
    return gpu;
}

Domain::GPUSnapshot snapshot(const std::string& id)
{
    Domain::GPUSnapshot snap;
    snap.gpuId = id;
    return snap;
}

std::vector<std::string> idsOf(const std::vector<GpuDrawEntry>& entries)
{
    std::vector<std::string> ids;
    ids.reserve(entries.size());
    for (const auto& entry : entries)
    {
        ids.emplace_back(entry.gpuId);
    }
    return ids;
}

TEST(GpuSectionDrawListTest, EnumeratedGpusAreDrawnInEnumerationOrderWithTheirSnapshots)
{
    Domain::GPUPublication publication;
    publication.gpuInfoKnown = true;
    publication.gpuInfo = {info("GPU2"), info("GPU0"), info("GPU1")};
    // Snapshot order does not decide the draw order.
    publication.snapshots = {snapshot("GPU1"), snapshot("GPU2"), snapshot("GPU0")};

    const auto entries = gpuDrawList(publication);

    ASSERT_EQ(idsOf(entries), (std::vector<std::string>{"GPU2", "GPU0", "GPU1"}));
    for (const auto& entry : entries)
    {
        ASSERT_NE(entry.info, nullptr);
        ASSERT_NE(entry.snapshot, nullptr);
        EXPECT_EQ(entry.info->id, entry.gpuId);
        EXPECT_EQ(entry.snapshot->gpuId, entry.gpuId);
    }
}

TEST(GpuSectionDrawListTest, AGpuMissingFromTheReadKeepsItsSlotAndTheOthersKeepTheirIds)
{
    // The tab used to draw only the snapshots and key each GPU's ImGui IDs by position, so when one
    // dropped out of a read the GPUs after it took over its collapsed state and chart layout (#1163).
    Domain::GPUPublication publication;
    publication.gpuInfoKnown = true;
    publication.gpuInfo = {info("GPU0"), info("GPU1"), info("GPU2")};
    publication.snapshots = {snapshot("GPU0"), snapshot("GPU1"), snapshot("GPU2")};
    const auto before = idsOf(gpuDrawList(publication));

    publication.snapshots = {snapshot("GPU0"), snapshot("GPU2")};
    const auto after = gpuDrawList(publication);

    EXPECT_EQ(idsOf(after), before);
    ASSERT_EQ(after.size(), 3U);
    EXPECT_NE(after[0].snapshot, nullptr);
    ASSERT_NE(after[1].info, nullptr);
    EXPECT_EQ(after[1].info->name, "Name GPU1");
    EXPECT_EQ(after[1].snapshot, nullptr);
    ASSERT_NE(after[2].snapshot, nullptr);
    EXPECT_EQ(after[2].snapshot->gpuId, "GPU2");
}

// #1171: the tab rebuilds the list every frame into storage it keeps, so the list must replace what
// that storage held -- not append to it -- and reuse its capacity.
TEST(GpuSectionDrawListTest, RebuildingIntoKeptStorageReplacesItsEntries)
{
    Domain::GPUPublication publication;
    publication.gpuInfoKnown = true;
    publication.gpuInfo = {info("GPU0"), info("GPU1"), info("GPU2")};
    publication.snapshots = {snapshot("GPU0"), snapshot("GPU1"), snapshot("GPU2")};

    std::vector<GpuDrawEntry> kept;
    gpuDrawList(publication, kept);
    ASSERT_EQ(idsOf(kept), (std::vector<std::string>{"GPU0", "GPU1", "GPU2"}));
    const auto* storage = kept.data();

    publication.gpuInfo = {info("GPU1")};
    publication.snapshots = {snapshot("GPU1")};
    gpuDrawList(publication, kept);

    EXPECT_EQ(idsOf(kept), (std::vector<std::string>{"GPU1"}));
    EXPECT_EQ(kept.data(), storage); // No reallocation for a shorter list
    EXPECT_EQ(idsOf(kept), idsOf(gpuDrawList(publication)));
}

// #1296 review: when every known GPU misses a read (one GPU, or all at once), the tab still draws
// each GPU's slot -- with "No reading" -- rather than collapsing into an empty state.
TEST(GpuSectionDrawListTest, WhenEveryGpuMissesTheReadEachKeepsItsSlot)
{
    Domain::GPUPublication publication;
    publication.gpuInfoKnown = true;
    publication.gpuInfo = {info("GPU0"), info("GPU1")};
    publication.snapshots = {};

    EXPECT_EQ(GpuSection::classifyEmptyState(true, true, publication.gpuInfo.size(), publication.snapshots.size()),
              GpuSection::EmptyReason::NoReadings);
    const auto entries = gpuDrawList(publication);
    ASSERT_EQ(idsOf(entries), (std::vector<std::string>{"GPU0", "GPU1"}));
    for (const auto& entry : entries)
    {
        ASSERT_NE(entry.info, nullptr);
        EXPECT_EQ(entry.snapshot, nullptr);
    }
}

TEST(GpuSectionDrawListTest, UnenumeratedSnapshotsFollowInPublishedOrder)
{
    Domain::GPUPublication publication;
    publication.gpuInfoKnown = true;
    publication.gpuInfo = {info("GPU0")};
    publication.snapshots = {snapshot("GPU0"), snapshot("late-a"), snapshot("late-b")};

    const auto entries = gpuDrawList(publication);

    ASSERT_EQ(idsOf(entries), (std::vector<std::string>{"GPU0", "late-a", "late-b"}));
    EXPECT_EQ(entries[1].info, nullptr);
    EXPECT_NE(entries[1].snapshot, nullptr);
}

TEST(GpuSectionDrawListTest, WithoutEnumerationTheSnapshotsAreDrawn)
{
    Domain::GPUPublication publication;
    publication.gpuInfoKnown = false;
    publication.snapshots = {snapshot("GPU0"), snapshot("GPU1")};

    const auto entries = gpuDrawList(publication);

    ASSERT_EQ(idsOf(entries), (std::vector<std::string>{"GPU0", "GPU1"}));
    EXPECT_EQ(entries[0].info, nullptr);
}

TEST(GpuSectionDrawListTest, ADuplicatedIdIsDrawnOnce)
{
    Domain::GPUPublication publication;
    publication.gpuInfoKnown = true;
    publication.gpuInfo = {info("GPU0"), info("GPU0")};
    publication.snapshots = {snapshot("GPU0")};

    EXPECT_EQ(idsOf(gpuDrawList(publication)), (std::vector<std::string>{"GPU0"}));
}

} // namespace
} // namespace App
