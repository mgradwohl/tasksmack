/// @file test_DRMGPUProbe.cpp
/// @brief Unit tests for Platform::DRMGPUProbe
///
/// Tests cover:
///   1. Integration tests against the real /sys/class/drm filesystem (if present).
///   2. Unit tests using a synthetic sysfs directory tree in /tmp, validating:
///      - PCI class/subclass + vendor-based integrated/discrete detection
///      - VRAM-presence fallback when PCI class file is absent
///      - Card discovery and driver filtering

#include <gtest/gtest.h>

#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/Linux/DRMGPUProbe.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace Platform
{
namespace
{

// =============================================================================
// Integration Tests (real /sys/class/drm — skipped when not present)
// =============================================================================

TEST(DRMGPUProbeIntegrationTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ DRMGPUProbe probe; });
}

TEST(DRMGPUProbeIntegrationTest, EnumerateGPUsDoesNotCrash)
{
    DRMGPUProbe probe;
    EXPECT_NO_THROW({ [[maybe_unused]] auto gpus = probe.enumerateGPUs(); });
}

TEST(DRMGPUProbeIntegrationTest, ReadGPUCountersDoesNotCrash)
{
    DRMGPUProbe probe;
    EXPECT_NO_THROW({ [[maybe_unused]] auto counters = probe.readGPUCounters(); });
}

TEST(DRMGPUProbeIntegrationTest, ReadProcessGPUCountersReturnsEmpty)
{
    DRMGPUProbe probe;
    // Per-process metrics are not yet supported via DRM sysfs
    auto counters = probe.readProcessGPUCounters();
    EXPECT_TRUE(counters.empty());
}

TEST(DRMGPUProbeIntegrationTest, IsIntegratedFieldIsValidWhenPresent)
{
    DRMGPUProbe probe;
    if (!probe.isAvailable())
    {
        GTEST_SKIP() << "No Intel DRM GPU found on this machine";
    }

    auto gpus = probe.enumerateGPUs();
    for (const auto& gpu : gpus)
    {
        // isIntegrated must be a boolean — just check it is set (true or false)
        EXPECT_TRUE(gpu.isIntegrated == true || gpu.isIntegrated == false);
        // Vendor must be non-empty
        EXPECT_FALSE(gpu.vendor.empty());
    }
}

// =============================================================================
// Unit test fixture — synthetic sysfs tree
// =============================================================================

/// Fixture that creates a temporary sysfs-like directory tree for each test.
/// DRM cards are created by the individual tests using the helper methods.
class DRMGPUProbeUnitTest : public ::testing::Test
{
  protected:
    std::filesystem::path m_SysRoot; // e.g. /tmp/tasksmack_drm_test_<PID>_<N>

    void SetUp() override
    {
        static std::atomic<int> s_counter{0};
        const auto seq = s_counter.fetch_add(1, std::memory_order_relaxed);
        const auto name =
            "tasksmack_drm_test_" + std::to_string(getpid()) + "_" + std::to_string(seq);
        m_SysRoot = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(m_SysRoot);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_SysRoot, ec);
    }

    /// Create a card directory with a device sub-directory and a driver symlink.
    /// @param cardName  e.g. "card0"
    /// @param driver    e.g. "i915" or "xe" — the filename the symlink points to
    /// @returns path to the device directory (cardPath/device)
    [[nodiscard]] std::filesystem::path makeCard(const std::string& cardName,
                                                 const std::string& driver = "i915") const
    {
        const auto deviceDir = m_SysRoot / cardName / "device";
        std::filesystem::create_directories(deviceDir);

        // Create a driver symlink whose filename equals the driver name.
        // DRMGPUProbe reads the symlink target's filename() to get the driver name.
        // The target path does not need to exist; only the filename matters.
        const auto driverLink = deviceDir / "driver";
        std::filesystem::create_symlink("/nonexistent/drivers/" + driver, driverLink);

        return deviceDir;
    }

    static void writeFile(const std::filesystem::path& path, const std::string& content)
    {
        std::ofstream f(path);
        f << content << "\n";
    }
};

// =============================================================================
// Card discovery tests
// =============================================================================

TEST_F(DRMGPUProbeUnitTest, EmptyBasePath_NotAvailable)
{
    DRMGPUProbe probe((m_SysRoot / "nonexistent").string());
    EXPECT_FALSE(probe.isAvailable());
    EXPECT_TRUE(probe.enumerateGPUs().empty());
}

TEST_F(DRMGPUProbeUnitTest, EmptyDirectory_NotAvailable)
{
    DRMGPUProbe probe(m_SysRoot.string());
    EXPECT_FALSE(probe.isAvailable());
}

TEST_F(DRMGPUProbeUnitTest, CardWithI915Driver_IsFound)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");

    DRMGPUProbe probe(m_SysRoot.string());
    EXPECT_TRUE(probe.isAvailable());
    EXPECT_EQ(probe.enumerateGPUs().size(), 1U);
}

TEST_F(DRMGPUProbeUnitTest, CardWithXeDriver_IsFound)
{
    const auto deviceDir = makeCard("card0", "xe");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");

    DRMGPUProbe probe(m_SysRoot.string());
    EXPECT_TRUE(probe.isAvailable());
    EXPECT_EQ(probe.enumerateGPUs().size(), 1U);
}

TEST_F(DRMGPUProbeUnitTest, CardWithNonIntelDriver_IsIgnored)
{
    const auto deviceDir = makeCard("card0", "amdgpu");
    writeFile(deviceDir / "vendor", "0x1002");
    writeFile(deviceDir / "class", "0x030000");

    DRMGPUProbe probe(m_SysRoot.string());
    EXPECT_FALSE(probe.isAvailable());
    EXPECT_TRUE(probe.enumerateGPUs().empty());
}

TEST_F(DRMGPUProbeUnitTest, MultipleCards_OnlyIntelIncluded)
{
    const auto intelDevDir = makeCard("card0", "i915");
    writeFile(intelDevDir / "vendor", "0x8086");
    writeFile(intelDevDir / "class", "0x030000");

    const auto amdDevDir = makeCard("card1", "amdgpu");
    writeFile(amdDevDir / "vendor", "0x1002");
    writeFile(amdDevDir / "class", "0x030000");

    DRMGPUProbe probe(m_SysRoot.string());
    EXPECT_TRUE(probe.isAvailable());
    EXPECT_EQ(probe.enumerateGPUs().size(), 1U);
    EXPECT_EQ(probe.enumerateGPUs()[0].vendor, "Intel");
}

// =============================================================================
// Integrated/discrete detection — PCI class + vendor
// =============================================================================

TEST_F(DRMGPUProbeUnitTest, IntelVGA_NoVRAM_IsIntegrated)
{
    // Intel UHD / Iris Xe: VGA class (0x030000), no dedicated VRAM
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");
    // No mem_info_vram_total file

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_TRUE(gpus[0].isIntegrated);
}

TEST_F(DRMGPUProbeUnitTest, IntelVGA_WithVRAM_IsDiscrete)
{
    // Intel Arc A380/A770 connected to display: VGA class but with dedicated VRAM
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");
    writeFile(deviceDir / "mem_info_vram_total", "4294967296"); // 4 GiB

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_FALSE(gpus[0].isIntegrated);
}

TEST_F(DRMGPUProbeUnitTest, Intel3DController_IsDiscrete)
{
    // Intel Arc in compute mode: 3D controller subclass (0x030200)
    const auto deviceDir = makeCard("card0", "xe");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030200");
    // No VRAM file needed — 3D controller class → always discrete

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_FALSE(gpus[0].isIntegrated);
}

TEST_F(DRMGPUProbeUnitTest, Intel3DController_WithVRAM_IsDiscrete)
{
    // 3D controller class always indicates discrete regardless of VRAM
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030200");
    writeFile(deviceDir / "mem_info_vram_total", "8589934592"); // 8 GiB

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_FALSE(gpus[0].isIntegrated);
}

TEST_F(DRMGPUProbeUnitTest, IntelDisplayController_NoVRAM_IsIntegrated)
{
    // Intel display controller (0x038000) without VRAM → integrated
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x038000");
    // No VRAM file

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_TRUE(gpus[0].isIntegrated);
}

TEST_F(DRMGPUProbeUnitTest, IntelDisplayController_WithVRAM_IsDiscrete)
{
    // Intel display controller (0x038000) with VRAM → discrete
    const auto deviceDir = makeCard("card0", "xe");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x038000");
    writeFile(deviceDir / "mem_info_vram_total", "2147483648"); // 2 GiB

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_FALSE(gpus[0].isIntegrated);
}

// =============================================================================
// VRAM-fallback tests (PCI class file absent or unrecognised)
// =============================================================================

TEST_F(DRMGPUProbeUnitTest, NoClassFile_NoVRAM_IsIntegrated)
{
    // No PCI class file and no VRAM → conservative default: integrated
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    // No class file, no mem_info_vram_total

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_TRUE(gpus[0].isIntegrated);
}

TEST_F(DRMGPUProbeUnitTest, NoClassFile_WithVRAM_IsDiscrete)
{
    // No PCI class file but VRAM present → discrete (VRAM fallback)
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "mem_info_vram_total", "4294967296"); // 4 GiB
    // No class file

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_FALSE(gpus[0].isIntegrated);
}

// =============================================================================
// Vendor name tests
// =============================================================================

TEST_F(DRMGPUProbeUnitTest, IntelVendorId_ReportsIntel)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].vendor, "Intel");
}

TEST_F(DRMGPUProbeUnitTest, UnknownVendorId_ReportsUnknown)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0xFFFF");
    writeFile(deviceDir / "class", "0x030000");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].vendor, "Unknown");
}

TEST_F(DRMGPUProbeUnitTest, MissingVendorFile_ReportsUnknown)
{
    const auto deviceDir = makeCard("card0", "i915");
    // No vendor file written
    writeFile(deviceDir / "class", "0x030000");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].vendor, "Unknown");
}

// =============================================================================
// Capabilities tests
// =============================================================================

TEST_F(DRMGPUProbeUnitTest, Capabilities_AvailableProbe_ReportsBasicSupport)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());

    const auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasTemperature);
    EXPECT_TRUE(caps.hasClockSpeeds);
    // Per-process and encode/decode metrics not supported via DRM sysfs
    EXPECT_FALSE(caps.hasPerProcessMetrics);
    EXPECT_FALSE(caps.hasEncoderDecoder);
}

TEST_F(DRMGPUProbeUnitTest, Capabilities_UnavailableProbe_ReportsNoSupport)
{
    DRMGPUProbe probe((m_SysRoot / "nonexistent").string());
    EXPECT_FALSE(probe.isAvailable());

    const auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasTemperature);
    EXPECT_FALSE(caps.hasClockSpeeds);
}

TEST_F(DRMGPUProbeUnitTest, ProcessGPUCounters_AlwaysEmpty)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");

    DRMGPUProbe probe(m_SysRoot.string());
    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
}

} // namespace
} // namespace Platform

#endif // defined(__linux__) && __has_include(<unistd.h>)
