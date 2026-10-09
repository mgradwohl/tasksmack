/// @file test_CpuDetailsText.cpp
/// @brief Tests for App::CpuDetailsText (#809): the Overview CPU Details block's rows -- what is
/// shown, hidden or marked unavailable -- and its responsive column-pair layout.

#include "App/Panels/CpuDetailsText.h"
#include "Domain/SystemSnapshot.h"
#include "Platform/CpuDetails.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace App
{
namespace
{

using CpuDetailsText::buildRows;
using CpuDetailsText::columnLayout;
using CpuDetailsText::Inputs;
using CpuDetailsText::Row;
using CpuDetailsText::UNAVAILABLE_TEXT;

[[nodiscard]] const Row* findRow(const std::vector<Row>& rows, std::string_view label)
{
    const auto it = std::ranges::find(rows, label, &Row::label);
    return (it == rows.end()) ? nullptr : &*it;
}

[[nodiscard]] Domain::SystemSnapshot windowsLikeSnapshot()
{
    Domain::SystemSnapshot snap;
    snap.cpuTotal.totalPercent = 12.5;
    snap.cpuTotal.systemPercent = 3.25;
    snap.cpuFreqMHz = 4250;
    snap.uptimeSeconds = 90'061; // 1d 1h 1m
    snap.coreCount = 32;
    snap.memoryTotalBytes = 32ULL * 1024 * 1024 * 1024;
    Platform::CpuDetails& cpu = snap.cpuDetails;
    cpu.sockets = 1;
    cpu.physicalCores = 24;
    cpu.performanceCores = 8;
    cpu.efficiencyCores = 16;
    cpu.logicalProcessors = 32;
    cpu.baseSpeedMHz = 3000;
    cpu.l1CacheBytes = 2'176ULL * 1024;
    cpu.l2CacheBytes = 32ULL * 1024 * 1024;
    cpu.l3CacheBytes = 36ULL * 1024 * 1024;
    cpu.virtualizationFirmwareEnabled = true;
    cpu.slatSupported = true;
    cpu.hypervisorPresent = true;
    cpu.vbsRunning = true;
    cpu.hvciEnabled = false;
    return snap;
}

[[nodiscard]] Inputs windowsInputs(const Domain::SystemSnapshot& snap)
{
    return Inputs{.snapshot = &snap,
                  .hasCpuFreq = true,
                  .hasUptime = true,
                  .hasVirtualizationInfo = true,
                  .processCount = 312,
                  .threadCount = 4'521.0,
                  .handleCount = 152'340.0,
                  .handlesAreFileDescriptors = false,
                  .totalVramBytes = 0};
}

TEST(CpuDetailsTextTest, LiveAndStaticFactsAreListed)
{
    const auto snap = windowsLikeSnapshot();
    const auto rows = buildRows(windowsInputs(snap));

    for (const std::string_view label : {"Utilization",
                                         "Kernel time",
                                         "Speed",
                                         "Processes",
                                         "Threads",
                                         "Handles",
                                         "Up time",
                                         "Base speed",
                                         "Sockets",
                                         "Cores",
                                         "Logical processors",
                                         "L1 cache",
                                         "L2 cache",
                                         "L3 cache",
                                         "Virtualization",
                                         "Hypervisor",
                                         "Virtualization-based security",
                                         "Memory integrity",
                                         "Memory"})
    {
        const Row* row = findRow(rows, label);
        ASSERT_NE(row, nullptr) << label;
        EXPECT_TRUE(row->available) << label;
        EXPECT_NE(row->value, UNAVAILABLE_TEXT) << label;
    }
    EXPECT_EQ(findRow(rows, "Speed")->value, "4.25 GHz");
    EXPECT_EQ(findRow(rows, "Base speed")->value, "3.00 GHz");
    EXPECT_EQ(findRow(rows, "Sockets")->value, "1");
    EXPECT_EQ(findRow(rows, "Up time")->value, "1d 01h"); // The duration alone, no "Up:" prefix
    EXPECT_EQ(findRow(rows, "Logical processors")->value, "32");
    EXPECT_EQ(findRow(rows, "Hypervisor")->value, "Detected");
    EXPECT_EQ(findRow(rows, "Virtualization-based security")->value, "Running");
    EXPECT_EQ(findRow(rows, "Memory integrity")->value, "Off");
    EXPECT_EQ(findRow(rows, "Virtualization")->value, "Enabled");
    EXPECT_FALSE(findRow(rows, "Virtualization")->tooltip.empty()); // SLAT on hover
    EXPECT_EQ(findRow(rows, "GPU memory"), nullptr);                // No discrete GPU
}

TEST(CpuDetailsTextTest, HybridCoresShowTheirSplit)
{
    const auto snap = windowsLikeSnapshot();
    const auto rows = buildRows(windowsInputs(snap));
    const Row* cores = findRow(rows, "Cores");
    ASSERT_NE(cores, nullptr);
    EXPECT_NE(cores->value.find("8 P + 16 E"), std::string::npos) << cores->value;
}

TEST(CpuDetailsTextTest, VirtualizationRowsAreHiddenWithoutTheCapability)
{
    const auto snap = windowsLikeSnapshot();
    Inputs in = windowsInputs(snap);
    in.hasVirtualizationInfo = false; // Linux
    in.handlesAreFileDescriptors = true;
    const auto rows = buildRows(in);
    for (const std::string_view label : {"Virtualization", "Hypervisor", "Virtualization-based security", "Memory integrity", "Handles"})
    {
        EXPECT_EQ(findRow(rows, label), nullptr) << label;
    }
    EXPECT_NE(findRow(rows, "FDs"), nullptr);
}

TEST(CpuDetailsTextTest, UnknownFactsAreMarkedUnavailableNotZero)
{
    Domain::SystemSnapshot snap; // Nothing known about the CPU
    Inputs in;
    in.snapshot = &snap;
    in.hasCpuFreq = true;
    in.hasUptime = true;
    in.hasVirtualizationInfo = true;
    const auto rows = buildRows(in);
    for (const std::string_view label : {"Speed",
                                         "Up time",
                                         "Processes",
                                         "Threads",
                                         "Handles",
                                         "Base speed",
                                         "Sockets",
                                         "Cores",
                                         "Logical processors",
                                         "L1 cache",
                                         "L2 cache",
                                         "L3 cache",
                                         "Virtualization",
                                         "Hypervisor",
                                         "Virtualization-based security",
                                         "Memory integrity"})
    {
        const Row* row = findRow(rows, label);
        ASSERT_NE(row, nullptr) << label;
        EXPECT_FALSE(row->available) << label;
        EXPECT_EQ(row->value, UNAVAILABLE_TEXT) << label;
        EXPECT_FALSE(row->tooltip.empty()) << label; // Says why
    }
}

TEST(CpuDetailsTextTest, LogicalProcessorsFallBackToTheSampledCount)
{
    Domain::SystemSnapshot snap;
    snap.coreCount = 12; // The probe samples 12, though its topology read gave nothing
    Inputs in;
    in.snapshot = &snap;
    const auto rows = buildRows(in);
    const Row* logical = findRow(rows, "Logical processors");
    ASSERT_NE(logical, nullptr);
    EXPECT_TRUE(logical->available);
    EXPECT_EQ(logical->value, "12");
}

TEST(CpuDetailsTextTest, VirtualizationFollowsTheFirmwareWithoutAHypervisor)
{
    auto snap = windowsLikeSnapshot();
    snap.cpuDetails.hypervisorPresent = false;
    snap.cpuDetails.virtualizationFirmwareEnabled = false;
    EXPECT_EQ(findRow(buildRows(windowsInputs(snap)), "Virtualization")->value, "Disabled");
    // A running hypervisor means it is enabled, whatever the firmware flag reads from its root partition
    snap.cpuDetails.hypervisorPresent = true;
    EXPECT_EQ(findRow(buildRows(windowsInputs(snap)), "Virtualization")->value, "Enabled");
}

TEST(CpuDetailsTextTest, CapabilitiesTheProbeLacksHideTheirRows)
{
    const auto snap = windowsLikeSnapshot();
    Inputs in = windowsInputs(snap);
    in.hasCpuFreq = false;
    in.hasUptime = false;
    const auto rows = buildRows(in);
    EXPECT_EQ(findRow(rows, "Speed"), nullptr);
    EXPECT_EQ(findRow(rows, "Up time"), nullptr);
}

TEST(CpuDetailsTextTest, GpuMemoryIsListedWithADiscreteGpu)
{
    const auto snap = windowsLikeSnapshot();
    Inputs in = windowsInputs(snap);
    in.totalVramBytes = 8ULL * 1024 * 1024 * 1024;
    EXPECT_NE(findRow(buildRows(in), "GPU memory"), nullptr);
}

TEST(CpuDetailsTextTest, NoSnapshotNoRows)
{
    EXPECT_TRUE(buildRows({}).empty());
}

// -----------------------------------------------------------------------------
// columnLayout
// -----------------------------------------------------------------------------

TEST(CpuDetailsTextTest, NarrowWidthStacksOnePair)
{
    const std::vector<float> labels(20, 100.0F);
    const std::vector<float> values(20, 80.0F);
    const auto layout = columnLayout(labels, values, 150.0F, 10.0F);
    EXPECT_EQ(layout.pairs, 1U);
    EXPECT_EQ(layout.rowsPerPair, 20U);
}

TEST(CpuDetailsTextTest, WiderWindowsUseMorePairsAndFewerRows)
{
    const std::vector<float> labels(20, 100.0F);
    const std::vector<float> values(20, 90.0F); // 200 px a pair with the 10 px extra
    EXPECT_EQ(columnLayout(labels, values, 400.0F, 10.0F).pairs, 2U);
    const auto four = columnLayout(labels, values, 800.0F, 10.0F);
    EXPECT_EQ(four.pairs, 4U);
    EXPECT_EQ(four.rowsPerPair, 5U);
    EXPECT_FLOAT_EQ(four.totalWidth, 800.0F);
    // Past the widest layout, the spare width goes to the value columns: the block spans the window
    const std::vector<float> few(4, 50.0F);
    const auto spread = columnLayout(few, few, 1000.0F, 0.0F);
    EXPECT_EQ(spread.pairs, 4U);
    EXPECT_FLOAT_EQ(spread.totalWidth, 1000.0F);
    EXPECT_FLOAT_EQ(spread.valueWidths[0], 50.0F + 150.0F);
}

TEST(CpuDetailsTextTest, EachPairIsSizedToItsOwnRows)
{
    // One long label (the last row) widens only its own pair, so three pairs still fit
    std::vector<float> labels(9, 60.0F);
    labels.back() = 200.0F;
    const std::vector<float> values(9, 50.0F);
    const auto layout = columnLayout(labels, values, 470.0F, 0.0F);
    EXPECT_EQ(layout.pairs, 3U);
    EXPECT_FLOAT_EQ(layout.labelWidths[0], 60.0F);
    EXPECT_FLOAT_EQ(layout.labelWidths[2], 200.0F);
    EXPECT_FLOAT_EQ(layout.totalWidth, 60.0F + 60.0F + 200.0F + (3 * 50.0F)); // Exactly fits: no slack to share
    // Sized to the widest row overall it would need 3 x 250 = 750 px
}

TEST(CpuDetailsTextTest, AWideWindowFitsEveryRowInThreeLinesAcrossTheFullWidth)
{
    // Widths like the Overview's at the default font: 22 rows of ~90 px labels (one long one,
    // "Virtualization-based security") and ~60 px values, gaps included
    std::vector<float> labels(22, 90.0F);
    labels[18] = 215.0F;
    const std::vector<float> values(22, 60.0F);
    constexpr float AVAILABLE = 1870.0F; // A 1900 px window's content width
    const auto layout = columnLayout(labels, values, AVAILABLE, 16.0F);
    EXPECT_LE(layout.rowsPerPair, 3U);
    EXPECT_FLOAT_EQ(layout.totalWidth, AVAILABLE); // The leftover width is shared out: the block spans it all
    float sum = 0.0F;
    for (std::size_t pair = 0; pair < layout.pairs; ++pair)
    {
        sum += layout.labelWidths[pair] + layout.valueWidths[pair] + 16.0F;
    }
    EXPECT_NEAR(sum, AVAILABLE, 0.01F);

    // At 1000 px the same rows take about three pairs of seven or eight lines
    const auto narrow = columnLayout(labels, values, 970.0F, 16.0F);
    EXPECT_GE(narrow.pairs, 3U);
    EXPECT_LE(narrow.rowsPerPair, 8U);
}

TEST(CpuDetailsTextTest, TheCollapsedSummaryListsTheKnownHeadlineFacts)
{
    auto snap = windowsLikeSnapshot();
    snap.cpuModel = "Test CPU";
    snap.cpuTotal.totalPercent = 4.2;
    EXPECT_EQ(CpuDetailsText::collapsedSummary(windowsInputs(snap)),
              "Test CPU \xC2\xB7 24 cores (8 P + 16 E) \xC2\xB7 3.00 GHz base \xC2\xB7 4.25 GHz \xC2\xB7 4.2%");
    // Unknown facts are left out, not shown as 0
    Domain::SystemSnapshot bare;
    bare.cpuModel = "Bare CPU";
    Inputs in;
    in.snapshot = &bare;
    EXPECT_EQ(CpuDetailsText::collapsedSummary(in), "Bare CPU \xC2\xB7 0%");
}

TEST(CpuDetailsTextTest, PairsAreCappedAndEvenedOut)
{
    const std::vector<float> wide(24, 50.0F);
    const auto capped = columnLayout(wide, wide, 10000.0F, 0.0F);
    EXPECT_EQ(capped.pairs, CpuDetailsText::MAX_COLUMN_PAIRS);
    EXPECT_EQ(capped.rowsPerPair, 3U); // 24 rows over 8 pairs
    // 9 rows over up to 8 pairs: 2 rows a pair, 5 pairs, none left empty
    const std::vector<float> nine(9, 50.0F);
    const auto even = columnLayout(nine, nine, 10000.0F, 0.0F);
    EXPECT_EQ(even.rowsPerPair, 2U);
    EXPECT_EQ(even.pairs, 5U);
}

TEST(CpuDetailsTextTest, DegenerateInputsFallBackToOnePair)
{
    const std::vector<float> five(5, 50.0F);
    EXPECT_EQ(columnLayout(five, five, 0.0F, 0.0F).pairs, 1U);
    EXPECT_EQ(columnLayout(five, five, std::numeric_limits<float>::quiet_NaN(), 0.0F).pairs, 1U);
    EXPECT_EQ(columnLayout({}, {}, 1000.0F, 0.0F).rowsPerPair, 0U);
}

} // namespace
} // namespace App
