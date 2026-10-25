/// @file test_GpuSectionEmptyState.cpp
/// @brief Tests for GpuSection::classifyEmptyState(), which decides what the GPU tab says when it
/// has nothing to chart (#927), and the GPU header helpers (VRAM total #1114, label #1117).

#include "App/Panels/GpuSection.h"
#include "Domain/GPUSnapshot.h"
#include "Platform/GPUTypes.h"
#include "UI/Format.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace App
{
namespace
{

using GpuSection::classifyEmptyState;
using GpuSection::EmptyReason;

Platform::GPUCapabilities allSensorsProbe()
{
    Platform::GPUCapabilities probe;
    probe.hasTemperature = true;
    probe.hasPowerMetrics = true;
    probe.hasClockSpeeds = true;
    probe.hasFanSpeed = true;
    probe.hasEncoderDecoder = true;
    probe.hasEngineUtilization = true;
    probe.hasPerProcessMetrics = true;
    return probe;
}

TEST(GpuSectionCapabilitiesTest, WithoutPerAdapterSensorsTheProbeCapabilitiesApply)
{
    const auto probe = allSensorsProbe();
    const auto caps = GpuSection::capabilitiesForGpu(probe, std::nullopt);

    EXPECT_TRUE(caps.hasTemperature);
    EXPECT_TRUE(caps.hasFanSpeed);
    EXPECT_TRUE(caps.hasEncoderDecoder);
}

TEST(GpuSectionCapabilitiesTest, AnAdapterWithoutSensorsGetsNoSensorSeries)
{
    // On a hybrid laptop NVML's capabilities belong to the NVIDIA GPU; the Intel iGPU, whose
    // sensors are not read, must not inherit them (#1040).
    const auto withoutSensors = GpuSection::capabilitiesForGpu(allSensorsProbe(), Platform::GPUCapabilities{});

    EXPECT_FALSE(withoutSensors.hasTemperature);
    EXPECT_FALSE(withoutSensors.hasPowerMetrics);
    EXPECT_FALSE(withoutSensors.hasClockSpeeds);
    EXPECT_FALSE(withoutSensors.hasFanSpeed);
    EXPECT_FALSE(withoutSensors.hasEncoderDecoder);
    // Utilization and per-process data come from PDH for every adapter.
    EXPECT_TRUE(withoutSensors.hasEngineUtilization);
    EXPECT_TRUE(withoutSensors.hasPerProcessMetrics);
}

TEST(GpuSectionCapabilitiesTest, EachAdapterKeepsOnlyTheSensorsItReports)
{
    // Two NVIDIA cards under one probe: one passively cooled, with no fan reading.
    Platform::GPUCapabilities fanless;
    fanless.hasTemperature = true;
    fanless.hasPowerMetrics = true;
    fanless.hasClockSpeeds = true;
    Platform::GPUCapabilities cooled = fanless;
    cooled.hasFanSpeed = true;

    const auto fanlessCaps = GpuSection::capabilitiesForGpu(allSensorsProbe(), fanless);
    const auto cooledCaps = GpuSection::capabilitiesForGpu(allSensorsProbe(), cooled);

    EXPECT_FALSE(fanlessCaps.hasFanSpeed);
    EXPECT_TRUE(fanlessCaps.hasTemperature);
    EXPECT_TRUE(cooledCaps.hasFanSpeed);
    // An adapter cannot gain a series the probe does not draw.
    Platform::GPUCapabilities noFanProbe = allSensorsProbe();
    noFanProbe.hasFanSpeed = false;
    EXPECT_FALSE(GpuSection::capabilitiesForGpu(noFanProbe, cooled).hasFanSpeed);
}
// GPUModel publishes on every successful refresh, even one that finds no GPU, so the absence of a
// publication is a missing probe or a failed read -- never an ordinary wait.
TEST(GpuSectionEmptyStateTest, NoPublicationIsUnavailable)
{
    EXPECT_EQ(classifyEmptyState(/*hasPublication=*/false, /*devicesKnown=*/false, /*deviceCount=*/0, /*snapshotCount=*/0),
              EmptyReason::Unavailable);
    EXPECT_EQ(classifyEmptyState(false, true, 2, 2), EmptyReason::Unavailable);
}

// Enumeration succeeded and found nothing: the one case that may say "no GPU detected".
TEST(GpuSectionEmptyStateTest, EnumeratedAndEmptyIsNoDevices)
{
    EXPECT_EQ(classifyEmptyState(true, true, 0, 0), EmptyReason::NoDevices);
}

// A reviewed defect: enumeration failed, so the empty device list means "could not look". Reporting
// that as "no GPU detected" would present a guess as a finding.
TEST(GpuSectionEmptyStateTest, FailedEnumerationIsUnavailableNotNoDevices)
{
    EXPECT_EQ(classifyEmptyState(true, false, 0, 0), EmptyReason::Unavailable);
}

// A reviewed defect: devices are known but the latest read returned nothing. That must not be
// reported as "no GPU detected" -- the GPU is there.
TEST(GpuSectionEmptyStateTest, KnownDevicesWithoutReadingsIsNoReadings)
{
    EXPECT_EQ(classifyEmptyState(true, true, 1, 0), EmptyReason::NoReadings);
    EXPECT_EQ(classifyEmptyState(true, true, 3, 0), EmptyReason::NoReadings);
}

TEST(GpuSectionEmptyStateTest, ReadingsMeanTheTabRenders)
{
    EXPECT_EQ(classifyEmptyState(true, true, 1, 1), EmptyReason::None);
    EXPECT_EQ(classifyEmptyState(true, true, 2, 1), EmptyReason::None);
    // Readings without a device list, or with enumeration having failed, still have something to
    // chart.
    EXPECT_EQ(classifyEmptyState(true, true, 0, 1), EmptyReason::None);
    EXPECT_EQ(classifyEmptyState(true, false, 0, 1), EmptyReason::None);
}

Domain::GPUSnapshot gpuSnapshot(bool isIntegrated, std::uint64_t memoryTotalBytes)
{
    Domain::GPUSnapshot snapshot;
    snapshot.isIntegrated = isIntegrated;
    snapshot.memoryTotalBytes = memoryTotalBytes;
    return snapshot;
}

constexpr std::uint64_t GIB = 1024ULL * 1024ULL * 1024ULL;

// #1114: an integrated GPU's memory total is borrowed system RAM, so the header's VRAM figure leaves
// it out. An iGPU-only laptop shows no VRAM; an iGPU + 8 GiB dGPU laptop shows 8 GiB, not ~24.
TEST(GpuSectionVramTest, IntegratedGpusAreNotCountedAsVram)
{
    const std::vector<Domain::GPUSnapshot> igpuOnly{gpuSnapshot(true, 16 * GIB)};
    EXPECT_EQ(GpuSection::totalDedicatedVramBytes(igpuOnly), 0U);

    const std::vector<Domain::GPUSnapshot> hybrid{gpuSnapshot(true, 16 * GIB), gpuSnapshot(false, 8 * GIB)};
    EXPECT_EQ(GpuSection::totalDedicatedVramBytes(hybrid), 8 * GIB);

    const std::vector<Domain::GPUSnapshot> twoDiscrete{gpuSnapshot(false, 8 * GIB), gpuSnapshot(false, 12 * GIB)};
    EXPECT_EQ(GpuSection::totalDedicatedVramBytes(twoDiscrete), 20 * GIB);

    EXPECT_EQ(GpuSection::totalDedicatedVramBytes({}), 0U);
}

// #1117: a sleeping GPU's header says so, and keeps one ImGui id however its label changes.
TEST(GpuSectionHeaderTest, LabelShowsKindVramAndSleep)
{
    const std::string vram = UI::Format::formatBytes(static_cast<double>(8 * GIB));
    const auto discrete = GpuSection::gpuHeaderLabel("*", "RTX", false, 8 * GIB, false);
    EXPECT_EQ(discrete, "* RTX, " + vram + " VRAM [Discrete]###gpuHeader");

    const auto sleeping = GpuSection::gpuHeaderLabel("*", "RTX", false, 8 * GIB, true);
    EXPECT_EQ(sleeping, "* RTX, " + vram + " VRAM [Discrete] (Sleeping)###gpuHeader");

    const auto integrated = GpuSection::gpuHeaderLabel("*", "Iris", true, 16 * GIB, false);
    EXPECT_EQ(integrated, "* Iris [Shared Memory]###gpuHeader");
}

} // namespace
} // namespace App
