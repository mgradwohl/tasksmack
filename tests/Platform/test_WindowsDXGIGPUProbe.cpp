/// @file test_WindowsDXGIGPUProbe.cpp
/// @brief Unit and smoke tests for Platform::DXGIGPUProbe
///
/// The vendor-ID/LUID-format/integrated-GPU decision logic is pure (no COM adapter
/// required) and is unit tested directly via DXGIGPUProbeMath.h. Enumeration/counter
/// tests below are integration smoke tests against whatever adapters are actually
/// present on the machine running CI.

#ifdef _WIN32

#include "Platform/Windows/DXGIGPUProbe.h"
#include "Platform/Windows/DXGIGPUProbeMath.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_set>

namespace Platform
{
namespace
{

// =============================================================================
// vendorIdToName: pure lookup, no hardware required.
// =============================================================================

TEST(VendorIdToNameTest, KnownVendorIdsMapCorrectly)
{
    EXPECT_EQ(vendorIdToName(0x10DE), "NVIDIA");
    EXPECT_EQ(vendorIdToName(0x1002), "AMD");
    EXPECT_EQ(vendorIdToName(0x1022), "AMD");
    EXPECT_EQ(vendorIdToName(0x8086), "Intel");
    EXPECT_EQ(vendorIdToName(0x8087), "Intel");
}

TEST(VendorIdToNameTest, UnknownVendorIdMapsToUnknown)
{
    EXPECT_EQ(vendorIdToName(0x1234), "Unknown");
    EXPECT_EQ(vendorIdToName(0), "Unknown");
}

// =============================================================================
// luidToPdhFormat: pure formatting, no hardware required.
// =============================================================================

TEST(LuidToPdhFormatTest, FormatsHighAndLowPartsAsHex)
{
    EXPECT_EQ(luidToPdhFormat(0, 0xD3A0), "GPU_0x00000000_0x0000D3A0");
    EXPECT_EQ(luidToPdhFormat(0xFFFFFFFF, 1), "GPU_0xFFFFFFFF_0x00000001");
}

// =============================================================================
// isIntegratedGPUFromDesc: pure vendor/VRAM-threshold decision logic, no hardware
// or COM adapter mocking required.
// =============================================================================

TEST(IsIntegratedGPUFromDescTest, SoftwareAdapterIsNeverIntegrated)
{
    constexpr uint32_t SOFTWARE_FLAG = 2;
    // Even an Intel vendor ID with tiny VRAM should report false for a software adapter.
    EXPECT_FALSE(isIntegratedGPUFromDesc(0x8086, SOFTWARE_FLAG, 0));
}

TEST(IsIntegratedGPUFromDescTest, IntelBelowThresholdIsIntegrated)
{
    constexpr uint64_t belowThreshold = (512ULL * 1024 * 1024) - 1;
    EXPECT_TRUE(isIntegratedGPUFromDesc(0x8086, 0, belowThreshold));
}

TEST(IsIntegratedGPUFromDescTest, IntelAtOrAboveThresholdIsDiscrete)
{
    constexpr uint64_t atThreshold = 512ULL * 1024 * 1024;
    EXPECT_FALSE(isIntegratedGPUFromDesc(0x8086, 0, atThreshold));
}

TEST(IsIntegratedGPUFromDescTest, AmdBelowThresholdIsIntegrated)
{
    constexpr uint64_t belowThreshold = (1024ULL * 1024 * 1024) - 1;
    EXPECT_TRUE(isIntegratedGPUFromDesc(0x1002, 0, belowThreshold));
}

TEST(IsIntegratedGPUFromDescTest, AmdAtOrAboveThresholdIsDiscrete)
{
    constexpr uint64_t atThreshold = 1024ULL * 1024 * 1024;
    EXPECT_FALSE(isIntegratedGPUFromDesc(0x1002, 0, atThreshold));
}

TEST(IsIntegratedGPUFromDescTest, NvidiaIsNeverIntegrated)
{
    EXPECT_FALSE(isIntegratedGPUFromDesc(0x10DE, 0, 0));
    EXPECT_FALSE(isIntegratedGPUFromDesc(0x10DE, 0, 0xFFFFFFFFFFFFFFFFULL));
}

TEST(IsIntegratedGPUFromDescTest, UnknownVendorIsNeverIntegrated)
{
    EXPECT_FALSE(isIntegratedGPUFromDesc(0x1234, 0, 0));
}

// =============================================================================
// Smoke Tests (real DXGI adapters, whatever this machine has)
// =============================================================================

TEST(DXGIGPUProbeTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ DXGIGPUProbe probe; });
}

TEST(DXGIGPUProbeTest, EnumerateGPUsReturnsWellFormedData)
{
    DXGIGPUProbe probe;
    auto gpus = probe.enumerateGPUs();

    std::unordered_set<std::string> ids;
    for (const auto& gpu : gpus)
    {
        EXPECT_FALSE(gpu.id.empty());
        EXPECT_FALSE(gpu.luidId.empty());
        EXPECT_TRUE(ids.insert(gpu.id).second) << "GPU ids should be unique";
    }
}

TEST(DXGIGPUProbeTest, ReadGPUCountersMatchesEnumeration)
{
    DXGIGPUProbe probe;
    auto gpus = probe.enumerateGPUs();
    auto counters = probe.readGPUCounters();

    EXPECT_EQ(gpus.size(), counters.size());
}

TEST(DXGIGPUProbeTest, ReadProcessGPUCountersIsAlwaysEmpty)
{
    // DXGI provides no per-process GPU metrics; that's PDH/NVML's job.
    DXGIGPUProbe probe;
    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
}

TEST(DXGIGPUProbeTest, CapabilitiesReflectDXGILimitations)
{
    DXGIGPUProbe probe;
    const auto caps = probe.capabilities();

    // DXGI never provides these, regardless of whether the factory initialized.
    EXPECT_FALSE(caps.hasTemperature);
    EXPECT_FALSE(caps.hasPowerMetrics);
    EXPECT_FALSE(caps.hasPerProcessMetrics);
    // supportsMultiGPU is only true once the DXGI factory is initialized; a machine
    // without a usable DXGI runtime is the only case where this would be false.
}

} // namespace
} // namespace Platform

#endif // _WIN32
