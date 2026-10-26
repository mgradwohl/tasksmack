/// @file test_WindowsGPUProbe.cpp
/// @brief Smoke tests for Platform::WindowsGPUProbe against this machine's real adapters
///
/// The pure merge/mapping logic (WindowsGPUProbeMath.h) is tested on every platform in
/// WindowsMath/test_WindowsGPUProbeMath.cpp.

#ifdef _WIN32

#include "Platform/GPUTypes.h"
#include "Platform/Windows/WindowsGPUProbe.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Platform
{
namespace
{

// ==========================================================================
// Basic Smoke Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, ConstructionDoesNotThrow)
{
    // Should not throw even if underlying probes are unavailable
    EXPECT_NO_THROW(const WindowsGPUProbe probe);
}

TEST(WindowsGPUProbeTest, BasicOperationsDoNotThrow)
{
    WindowsGPUProbe probe;
    EXPECT_NO_THROW([[maybe_unused]] const auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] const auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] const auto process = probe.readProcessGPUCounters());
}

// ==========================================================================
// Enumeration Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, EnumerateGPUsSmokeReturnsWellFormedDataWhenPresent)
{
    WindowsGPUProbe probe;
    const auto gpus = probe.enumerateGPUs();

    std::unordered_set<std::string> ids;

    // If GPUs are present, validate their fields
    for (const auto& gpu : gpus)
    {
        EXPECT_FALSE(gpu.id.empty()) << "GPU id should not be empty";
        EXPECT_FALSE(gpu.name.empty()) << "GPU name should not be empty";
        EXPECT_TRUE(ids.insert(gpu.id).second) << "GPU ids should be unique";
        // luidId may be present for DXGI enumeration
    }
}

TEST(WindowsGPUProbeTest, EnumerateGPUsIsDeterministic)
{
    WindowsGPUProbe probe;

    auto gpus1 = probe.enumerateGPUs();
    auto gpus2 = probe.enumerateGPUs();

    // Enumeration should be deterministic (same list on consecutive calls)
    EXPECT_EQ(gpus1.size(), gpus2.size()) << "GPU count should be consistent across calls";

    // IDs should match in order
    for (std::size_t i = 0; i < gpus1.size(); ++i)
    {
        EXPECT_EQ(gpus1[i].id, gpus2[i].id) << "GPU id should be consistent at index " << i;
        EXPECT_EQ(gpus1[i].name, gpus2[i].name) << "GPU name should be consistent at index " << i;
    }
}

// ==========================================================================
// Counter Reading Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, ReadGPUCountersReturnsValidList)
{
    WindowsGPUProbe probe;

    // Enumerate first (required for LUID mapping in counter reading)
    const auto gpus = probe.enumerateGPUs();

    // Now read counters
    const auto counters = probe.readGPUCounters();

    // Counter list should match GPU list size (or be empty if no GPUs)
    if (gpus.empty())
    {
        EXPECT_EQ(counters.size(), 0UL) << "No counters should be returned if no GPUs enumerated";
    }
    else
    {
        EXPECT_EQ(counters.size(), gpus.size()) << "Counter count should match GPU count";

        // Validate counter fields
        for (const auto& counter : counters)
        {
            EXPECT_FALSE(counter.gpuId.empty()) << "Counter gpuId should not be empty";
            EXPECT_GE(counter.utilizationPercent, 0.0) << "Utilization should be >= 0";
            EXPECT_LE(counter.utilizationPercent, 100.0) << "Utilization should be <= 100";
        }
    }
}

TEST(WindowsGPUProbeTest, ReadGPUCountersAfterEnumerateIsConsistent)
{
    WindowsGPUProbe probe;

    const auto gpus = probe.enumerateGPUs();
    const auto counters1 = probe.readGPUCounters();
    const auto counters2 = probe.readGPUCounters();

    // Counter list should be consistent
    EXPECT_EQ(counters1.size(), counters2.size()) << "Counter count should be consistent across calls";

    // GPUs should remain enumerated
    const auto gpus2 = probe.enumerateGPUs();
    EXPECT_EQ(gpus.size(), gpus2.size()) << "GPU count should not change";
}

// ==========================================================================
// Process Counter Reading Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, ReadProcessGPUCountersReturnsValidList)
{
    WindowsGPUProbe probe;
    const auto counters = probe.readProcessGPUCounters();

    // Per-process GPU counters may be empty (no GPU-using processes)
    // or populated if processes are using GPU resources
    for (const auto& counter : counters)
    {
        EXPECT_GE(counter.pid, 0) << "Process ID should be non-negative";
        // Other fields may vary depending on availability
    }
}

// Per-process samples are this machine's listed GPUs' only (#1826): PDH also has instances for an
// NPU (an MCDM adapter DXGI never lists) and the Basic Render Driver, which must not be counted as
// GPU work or reach ProcessModel as an unknown "GPU_0x..." device.
TEST(WindowsGPUProbeTest, ProcessCountersAreOnlyForListedGPUs)
{
    WindowsGPUProbe probe;
    std::unordered_set<std::string> listedLuids;
    for (const auto& gpu : probe.enumerateGPUs())
    {
        listedLuids.insert(gpu.luidId);
    }
    static_cast<void>(probe.readProcessGPUCounters()); // PDH's first collect only warms up its rates
    for (const auto& counter : probe.readProcessGPUCounters())
    {
        EXPECT_TRUE(listedLuids.contains(counter.gpuId)) << "pid " << counter.pid << " on " << counter.gpuId;
    }
    // Those adapters' samples are kept apart instead (#1826)
    for (const auto& usage : probe.otherAdapterUsage())
    {
        EXPECT_FALSE(listedLuids.contains(usage.adapterLuid)) << "pid " << usage.pid << " on " << usage.adapterLuid;
    }
}

// #1799: this machine's NPUs and compute accelerators (DXCore) are listed after its GPUs with their
// kind, each adapter once, and a per-process sample is tagged with its adapter's kind. Each adapter
// is printed (kind, name, id, LUID, driver version) for a look at the real hardware: on a Core
// Ultra 7 255H, "Intel(R) AI Boost" as an NPU with its PnP driver version.
TEST(WindowsGPUProbeTest, AcceleratorsAreListedAfterTheGPUsWithTheirKind)
{
    WindowsGPUProbe probe;
    const auto adapters = probe.enumerateGPUs();
    std::unordered_map<std::string, DeviceKind> kinds;
    bool seenAccelerator = false;
    for (const auto& adapter : adapters)
    {
        const char* kind = "ComputeAccelerator";
        if (adapter.kind == DeviceKind::Gpu)
        {
            kind = "Gpu";
        }
        else if (adapter.kind == DeviceKind::Npu)
        {
            kind = "Npu";
        }
        std::cout << "[ adapter  ] " << kind << " | " << adapter.name << " | " << adapter.id << " | " << adapter.luidId << " | driver "
                  << adapter.driverVersion << '\n';
        EXPECT_FALSE(seenAccelerator && adapter.kind == DeviceKind::Gpu) << adapter.name << ": a GPU after an accelerator";
        seenAccelerator = seenAccelerator || adapter.kind != DeviceKind::Gpu;
        EXPECT_FALSE(adapter.luidId.empty()) << adapter.name;
        EXPECT_TRUE(kinds.emplace(adapter.luidId, adapter.kind).second) << adapter.name << " listed twice";
        EXPECT_FALSE(adapter.driverVersion.empty()) << adapter.name;
    }
    static_cast<void>(probe.readProcessGPUCounters()); // PDH's first collect only warms up its rates
    for (const auto& counter : probe.readProcessGPUCounters())
    {
        const auto kind = kinds.find(counter.gpuId);
        ASSERT_NE(kind, kinds.end()) << counter.gpuId;
        EXPECT_EQ(counter.kind, kind->second) << "pid " << counter.pid << " on " << counter.gpuId;
    }
}

// ==========================================================================
// State Consistency Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, ProbeStatePersistsBetweenCalls)
{
    WindowsGPUProbe probe;

    // First enumeration
    const auto gpus1 = probe.enumerateGPUs();
    const auto caps1 = probe.capabilities();

    // Second enumeration
    const auto gpus2 = probe.enumerateGPUs();
    const auto caps2 = probe.capabilities();

    // State should be consistent
    EXPECT_EQ(gpus1.size(), gpus2.size());
    // Capabilities should match (at least structure-wise)
    EXPECT_EQ(caps1.hasTemperature, caps2.hasTemperature);
    EXPECT_EQ(caps1.hasPowerMetrics, caps2.hasPowerMetrics);
}

TEST(WindowsGPUProbeTest, CapabilitiesReturnsConsistentStructure)
{
    const WindowsGPUProbe probe;
    const auto caps = probe.capabilities();

    // The public contract only guarantees stable self-consistency and the
    // per-process/engine-utilization relationship.
    EXPECT_EQ(caps.hasPerProcessMetrics, caps.hasEngineUtilization);
}

} // namespace
} // namespace Platform

#endif // _WIN32
