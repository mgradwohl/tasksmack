/// @file test_GpuSectionDrawList.cpp
/// @brief Tests for GpuSection::gpuDrawList(), which decides which GPUs the GPU tab draws, in what
/// order, and under which IDs (#1163).

#include "App/Panels/GpuSection.h"
#include "Domain/GPUModel.h"
#include "Domain/GPUSnapshot.h"
#include "Mocks/MockGPUProbe.h"
#include "Platform/GPUTypes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
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

// #1799: the GPU tab and the Overview's VRAM total draw from a GPU model's publication, which lists
// an NPU apart from the GPUs, so neither shows nor counts it before the NPU tab exists.
TEST(GpuSectionDrawListTest, AnNpuIsNeitherDrawnNorCountedAsAGpu)
{
    constexpr std::uint64_t GIB = 1ULL << 30U;
    auto probe = std::make_unique<TestMocks::MockGPUProbe>();
    probe->withGPU("dGPU", "Discrete GPU", "NVIDIA", false)
        .withMemory("dGPU", GIB, 8 * GIB)
        .withAccelerator("NPU", "Intel(R) AI Boost", "GPU_0x00000000_0x00018103")
        .withGPUCounters("NPU",
                         []
                         {
                             auto counters = TestMocks::makeGPUCounters("NPU", 10.0, GIB, 16 * GIB);
                             counters.kind = Platform::DeviceKind::Npu;
                             return counters;
                         }());
    Domain::GPUModel model(std::move(probe));
    model.refresh();
    const auto publication = model.publication();
    ASSERT_NE(publication, nullptr);

    EXPECT_EQ(idsOf(gpuDrawList(*publication)), (std::vector<std::string>{"dGPU"}));
    EXPECT_EQ(GpuSection::totalDedicatedVramBytes(publication->snapshots), 8 * GIB) << "Without the NPU's 16 GiB";
    ASSERT_EQ(publication->acceleratorSnapshots.size(), 1U);
    EXPECT_EQ(publication->acceleratorSnapshots[0].gpuId, "NPU");
}

// #1799: the NPU tab draws the publication's accelerators the way the GPU tab draws its GPUs:
// enumeration order, a missing read keeping its slot, then any read enumeration did not list.
TEST(GpuSectionDrawListTest, TheNpuTabDrawsTheAcceleratorsAlone)
{
    Domain::GPUPublication publication;
    publication.gpuInfoKnown = true;
    publication.gpuInfo = {info("iGPU")};
    publication.snapshots = {snapshot("iGPU")};
    publication.acceleratorInfo = {info("NPU1"), info("NPU0")};
    publication.acceleratorSnapshots = {snapshot("NPU0"), snapshot("Unlisted")};

    const std::vector<GpuDrawEntry> entries = GpuSection::acceleratorDrawList(publication);
    EXPECT_EQ(idsOf(entries), (std::vector<std::string>{"NPU1", "NPU0", "Unlisted"}));
    ASSERT_EQ(entries.size(), 3U);
    EXPECT_EQ(entries[0].snapshot, nullptr) << "Enumerated, but not read: keeps its slot";
    EXPECT_EQ(entries[1].snapshot, publication.acceleratorSnapshots.data());
    EXPECT_EQ(entries[2].info, nullptr);
    EXPECT_EQ(idsOf(gpuDrawList(publication)), (std::vector<std::string>{"iGPU"})) << "Nor does the GPU tab draw them";
}

TEST(GpuSectionDrawListTest, TheNpuTabIsShownOnlyWithAnAccelerator)
{
    EXPECT_FALSE(GpuSection::showAcceleratorTab(nullptr));
    Domain::GPUPublication publication;
    publication.gpuInfoKnown = true;
    publication.gpuInfo = {info("iGPU")};
    publication.snapshots = {snapshot("iGPU")};
    EXPECT_FALSE(GpuSection::showAcceleratorTab(&publication)) << "A GPU alone: no NPU tab (and every Linux system, #1827)";
    publication.acceleratorInfo = {info("NPU")};
    EXPECT_TRUE(GpuSection::showAcceleratorTab(&publication));
    publication.acceleratorInfo.clear();
    publication.acceleratorSnapshots = {snapshot("NPU")};
    EXPECT_TRUE(GpuSection::showAcceleratorTab(&publication)) << "Read, though enumeration did not list it";
}

TEST(GpuSectionDrawListTest, AnAcceleratorIsLabelledWithItsKindAndDriver)
{
    EXPECT_EQ(GpuSection::deviceKindLabel(Platform::DeviceKind::Npu), "NPU");
    EXPECT_EQ(GpuSection::deviceKindLabel(Platform::DeviceKind::ComputeAccelerator), "Compute accelerator");
    EXPECT_EQ(GpuSection::deviceKindLabel(Platform::DeviceKind::Gpu), "GPU");

    EXPECT_EQ(GpuSection::acceleratorHeaderLabel("*", "Intel(R) AI Boost", Platform::DeviceKind::Npu, false),
              "* Intel(R) AI Boost [NPU]###gpuHeader");
    EXPECT_EQ(GpuSection::acceleratorHeaderLabel("*", "Contoso", Platform::DeviceKind::ComputeAccelerator, true),
              "* Contoso [Compute accelerator] (Sleeping)###gpuHeader");

    Platform::GPUInfo npu = info("NPU");
    npu.kind = Platform::DeviceKind::Npu;
    npu.vendor = "Intel";
    npu.driverVersion = "32.0.100.4778";
    npu.memoryIsShared = true;
    EXPECT_EQ(GpuSection::acceleratorDetailsText(npu), "Kind: NPU    Vendor: Intel    Driver: 32.0.100.4778    Memory: Shared");

    Platform::GPUInfo unknown = info("ACC");
    unknown.kind = Platform::DeviceKind::ComputeAccelerator;
    unknown.vendor = "Unknown";
    EXPECT_EQ(GpuSection::acceleratorDetailsText(unknown), "Kind: Compute accelerator    Driver: Unknown    Memory: Dedicated");
}

} // namespace
} // namespace App
