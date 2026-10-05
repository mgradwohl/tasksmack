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
#include <tuple>

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
    ASSERT_FALSE(gpus.empty()) << "Expected at least one GPU when probe is available";
    for (const auto& gpu : gpus)
    {
        // Vendor and id must be populated by the detection logic
        EXPECT_FALSE(gpu.vendor.empty());
        EXPECT_FALSE(gpu.id.empty());
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
        const auto name = "tasksmack_drm_test_" + std::to_string(getpid()) + "_" + std::to_string(seq);
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
    [[nodiscard]] std::filesystem::path makeCard(const std::string& cardName, const std::string& driver = "i915") const
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

    /// Like makeCard(), but with cardName/device a symlink to a PCI device directory named by its
    /// address, as in real sysfs (/sys/class/drm/card0/device -> .../0000:00:02.0).
    /// @returns path to the PCI device directory
    [[nodiscard]] std::filesystem::path
    makeCardAt(const std::string& cardName, const std::string& pciAddress, const std::string& driver = "i915") const
    {
        const auto pciDir = m_SysRoot / "pci" / pciAddress;
        std::filesystem::create_directories(pciDir);
        std::filesystem::create_directories(m_SysRoot / cardName);
        std::filesystem::create_directory_symlink(pciDir, m_SysRoot / cardName / "device");
        std::filesystem::create_symlink("/nonexistent/drivers/" + driver, pciDir / "driver");
        writeFile(pciDir / "vendor", "0x8086");
        writeFile(pciDir / "class", "0x030000");
        return pciDir;
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

TEST_F(DRMGPUProbeUnitTest, FileAsBasePath_NotAvailable)
{
    // Passing a regular file instead of a directory must not throw; probe is simply unavailable.
    const auto filePath = m_SysRoot / "not_a_directory";
    writeFile(filePath, "regular file");

    EXPECT_NO_THROW({
        DRMGPUProbe probe(filePath.string());
        EXPECT_FALSE(probe.isAvailable());
        EXPECT_TRUE(probe.enumerateGPUs().empty());
    });
}

TEST_F(DRMGPUProbeUnitTest, UnreadableDirectory_NotAvailable)
{
    if (getuid() == 0)
    {
        GTEST_SKIP() << "Cannot test EACCES as root";
    }

    // A mode-000 directory triggers an error-code path in directory_iterator.
    // The probe must not throw and must report unavailable.
    const auto lockedDir = m_SysRoot / "locked";
    std::filesystem::create_directories(lockedDir);
    std::filesystem::permissions(lockedDir, std::filesystem::perms::none);

    EXPECT_NO_THROW({
        DRMGPUProbe probe(lockedDir.string());
        EXPECT_FALSE(probe.isAvailable());
        EXPECT_TRUE(probe.enumerateGPUs().empty());
    });

    // Restore permissions so TearDown can remove it
    std::filesystem::permissions(lockedDir, std::filesystem::perms::all);
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

TEST_F(DRMGPUProbeUnitTest, MissingVendorFile_VGA_NoVRAM_IsIntegrated)
{
    // Vendor file missing (returns empty string → vendor ID 0).
    // Card is driven by i915 (Intel), so conservative fallback should treat it as integrated.
    const auto deviceDir = makeCard("card0", "i915");
    // No vendor file
    writeFile(deviceDir / "class", "0x030000"); // VGA compatible

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_TRUE(gpus[0].isIntegrated);
}

TEST_F(DRMGPUProbeUnitTest, MissingVendorFile_VGA_WithVRAM_IsDiscrete)
{
    // Vendor file missing but VRAM present: conservative unknown vendor should still classify
    // as discrete when VRAM is available (VRAM is unambiguous evidence of a dedicated GPU).
    const auto deviceDir = makeCard("card0", "i915");
    // No vendor file
    writeFile(deviceDir / "class", "0x030000");                 // VGA compatible
    writeFile(deviceDir / "mem_info_vram_total", "4294967296"); // 4 GiB

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_FALSE(gpus[0].isIntegrated);
}

TEST_F(DRMGPUProbeUnitTest, MissingVendorFile_DisplayController_NoVRAM_IsIntegrated)
{
    // Vendor file missing, display controller class, no VRAM → conservative: integrated.
    const auto deviceDir = makeCard("card0", "i915");
    // No vendor file
    writeFile(deviceDir / "class", "0x038000"); // Display controller (non-VGA)

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_TRUE(gpus[0].isIntegrated);
}

TEST_F(DRMGPUProbeUnitTest, MissingVendorFile_DisplayController_WithVRAM_IsDiscrete)
{
    // Vendor file missing, display controller class, VRAM present → discrete.
    const auto deviceDir = makeCard("card0", "xe");
    // No vendor file
    writeFile(deviceDir / "class", "0x038000");                 // Display controller (non-VGA)
    writeFile(deviceDir / "mem_info_vram_total", "2147483648"); // 2 GiB

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

TEST_F(DRMGPUProbeUnitTest, Capabilities_MultipleIntelGPUs_SupportsMultiGPU)
{
    for (const auto* card : {"card0", "card1"})
    {
        const auto deviceDir = makeCard(card, "i915");
        writeFile(deviceDir / "vendor", "0x8086");
        writeFile(deviceDir / "class", "0x030000");
    }

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    EXPECT_EQ(probe.enumerateGPUs().size(), 2U);

    const auto caps = probe.capabilities();
    EXPECT_TRUE(caps.supportsMultiGPU);
}

// =============================================================================
// readGPUCounters — sysfs file reading paths
// =============================================================================

/// Helper that creates a hwmon directory tree: device/hwmon/hwmon0/
static void makeHwmon(const std::filesystem::path& deviceDir, const std::string& hwmonName = "hwmon0")
{
    std::filesystem::create_directories(deviceDir / "hwmon" / hwmonName);
}

static void writeHwmonFile(const std::filesystem::path& deviceDir,
                           const std::string& hwmonName,
                           const std::string& filename,
                           const std::string& content)
{
    std::ofstream f(deviceDir / "hwmon" / hwmonName / filename);
    f << content << "\n";
}

TEST_F(DRMGPUProbeUnitTest, ReadGPUCounters_NoSysfsFiles_ReturnsZeros)
{
    // Card with no optional sysfs files — counter fields should all be zero/default.
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].temperatureC, 0);
    EXPECT_EQ(counters[0].gpuClockMHz, 0U);
    EXPECT_EQ(counters[0].memoryUsedBytes, 0ULL);
    EXPECT_EQ(counters[0].memoryTotalBytes, 0ULL);
    // No clock file and no hwmon: both unread, so the history has gaps rather than a real-looking 0
    // (capabilities() advertises temperature for every card) (#1111).
    EXPECT_FALSE(counters[0].gpuClockAvailable);
    EXPECT_FALSE(counters[0].temperatureAvailable);
    EXPECT_FALSE(counters[0].utilizationAvailable); // DRM never reads utilization (#1115)
}

TEST_F(DRMGPUProbeUnitTest, ReadGPUCounters_HwmonTemperature_IsRead)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");

    // Create hwmon directory with temp1_input in millidegrees Celsius
    makeHwmon(deviceDir);
    writeHwmonFile(deviceDir, "hwmon0", "temp1_input", "65000"); // 65 °C

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].temperatureC, 65);
    EXPECT_TRUE(counters[0].temperatureAvailable);
}

TEST_F(DRMGPUProbeUnitTest, ReadGPUCounters_HwmonZeroTemp_IsIgnored)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");

    makeHwmon(deviceDir);
    writeHwmonFile(deviceDir, "hwmon0", "temp1_input", "0");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    // A zero reading from the file should leave temperatureC at 0, marked unread (#1111)
    EXPECT_EQ(counters[0].temperatureC, 0);
    EXPECT_FALSE(counters[0].temperatureAvailable);
}

TEST_F(DRMGPUProbeUnitTest, ReadGPUCounters_GpuFrequency_IsRead)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");

    // gt_cur_freq_mhz lives under the card directory (not device/)
    const auto cardDir = m_SysRoot / "card0";
    writeFile(cardDir / "gt_cur_freq_mhz", "1200");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].gpuClockMHz, 1200U);
    EXPECT_TRUE(counters[0].gpuClockAvailable);
}

TEST_F(DRMGPUProbeUnitTest, ReadGPUCounters_ZeroFrequency_IsIgnored)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");

    const auto cardDir = m_SysRoot / "card0";
    writeFile(cardDir / "gt_cur_freq_mhz", "0");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].gpuClockMHz, 0U);
}

TEST_F(DRMGPUProbeUnitTest, ReadGPUCounters_VramUsedAndTotal_AreRead)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030200");                 // discrete (3D controller)
    writeFile(deviceDir / "mem_info_vram_used", "2147483648");  // 2 GiB used
    writeFile(deviceDir / "mem_info_vram_total", "4294967296"); // 4 GiB total

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].memoryUsedBytes, 2147483648ULL);
    EXPECT_EQ(counters[0].memoryTotalBytes, 4294967296ULL);
}

TEST_F(DRMGPUProbeUnitTest, ReadGPUCounters_ZeroVram_IsIgnored)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");
    writeFile(deviceDir / "mem_info_vram_used", "0");
    writeFile(deviceDir / "mem_info_vram_total", "0");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].memoryUsedBytes, 0ULL);
    EXPECT_EQ(counters[0].memoryTotalBytes, 0ULL);
}

TEST_F(DRMGPUProbeUnitTest, ReadGPUCounters_AllSysfsFiles_AllFieldsPopulated)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030200"); // discrete

    makeHwmon(deviceDir);
    writeHwmonFile(deviceDir, "hwmon0", "temp1_input", "72000"); // 72 °C

    const auto cardDir = m_SysRoot / "card0";
    writeFile(cardDir / "gt_cur_freq_mhz", "950");

    writeFile(deviceDir / "mem_info_vram_used", "1073741824");  // 1 GiB
    writeFile(deviceDir / "mem_info_vram_total", "8589934592"); // 8 GiB

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].temperatureC, 72);
    EXPECT_EQ(counters[0].gpuClockMHz, 950U);
    EXPECT_EQ(counters[0].memoryUsedBytes, 1073741824ULL);
    EXPECT_EQ(counters[0].memoryTotalBytes, 8589934592ULL);
}

TEST_F(DRMGPUProbeUnitTest, ReadGPUCounters_MultipleGPUs_EachGetsOwnCounters)
{
    // card0: 65 °C, 1200 MHz, no VRAM
    const auto dev0 = makeCard("card0", "i915");
    writeFile(dev0 / "vendor", "0x8086");
    writeFile(dev0 / "class", "0x030000");
    makeHwmon(dev0);
    writeHwmonFile(dev0, "hwmon0", "temp1_input", "65000");
    writeFile(m_SysRoot / "card0" / "gt_cur_freq_mhz", "1200");

    // card1: 72 °C, 950 MHz, 4 GiB VRAM
    const auto dev1 = makeCard("card1", "xe");
    writeFile(dev1 / "vendor", "0x8086");
    writeFile(dev1 / "class", "0x030200");
    makeHwmon(dev1);
    writeHwmonFile(dev1, "hwmon0", "temp1_input", "72000");
    writeFile(m_SysRoot / "card1" / "gt_cur_freq_mhz", "950");
    writeFile(dev1 / "mem_info_vram_used", "1073741824");
    writeFile(dev1 / "mem_info_vram_total", "4294967296");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 2U);

    // Find card0 counters (order may vary depending on directory iteration)
    const auto* c0 = counters[0].gpuClockMHz == 1200U ? &counters[0] : &counters[1];
    const auto* c1 = counters[0].gpuClockMHz == 1200U ? &counters[1] : &counters[0];

    EXPECT_EQ(c0->temperatureC, 65);
    EXPECT_EQ(c0->gpuClockMHz, 1200U);
    EXPECT_EQ(c0->memoryTotalBytes, 0ULL); // integrated, no VRAM

    EXPECT_EQ(c1->temperatureC, 72);
    EXPECT_EQ(c1->gpuClockMHz, 950U);
    EXPECT_EQ(c1->memoryTotalBytes, 4294967296ULL);
}

// =============================================================================
// uevent device name parsing tests
// =============================================================================

TEST_F(DRMGPUProbeUnitTest, EnumerateGPUs_UeventWithPciId_ParsedCorrectly)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");
    // uevent with PCI_ID entry → probe should extract "8086:9A49" as the id
    writeFile(deviceDir / "uevent", "PCI_ID=8086:9A49\nDRIVER=i915\n");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_TRUE(gpus[0].name.contains("Intel"));
    EXPECT_TRUE(gpus[0].name.contains("8086"));
}

TEST_F(DRMGPUProbeUnitTest, EnumerateGPUs_UeventWithoutPciId_FallbackToIds)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");
    writeFile(deviceDir / "device", "0x9A49"); // PCI device ID
    // uevent without PCI_ID → probe falls back to vendor:device string
    writeFile(deviceDir / "uevent", "DRIVER=i915\nPCI_SLOT_NAME=0000:00:02.0\n");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_FALSE(gpus[0].name.empty());
}

// =============================================================================
// Malformed sysfs content tests
// =============================================================================

TEST_F(DRMGPUProbeUnitTest, MalformedHexClass_TreatedAsUnrecognised)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "not_a_hex_value"); // malformed

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());

    // Should not crash and should still enumerate the card
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    // Unrecognised class with no VRAM → conservative default: integrated
    EXPECT_TRUE(gpus[0].isIntegrated);
}

TEST_F(DRMGPUProbeUnitTest, MalformedFrequencyFile_TreatedAsZero)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");
    writeFile(m_SysRoot / "card0" / "gt_cur_freq_mhz", "not_a_number");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    // Malformed value → treated as zero → field left at 0
    EXPECT_EQ(counters[0].gpuClockMHz, 0U);
}

// =============================================================================
// Integrated/discrete by PCI bus (#1113)
// =============================================================================

// i915 exposes no dedicated-memory file, so an Arc on i915 (a VGA controller behind a PCIe switch)
// used to classify as integrated. Its non-zero bus says it is discrete.
TEST_F(DRMGPUProbeUnitTest, IntelVGA_OnNonZeroBus_NoVramFile_IsDiscrete)
{
    std::ignore = makeCardAt("card1", "0000:03:00.0", "i915");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].id, "0000:03:00.0");
    EXPECT_FALSE(gpus[0].isIntegrated);
}

TEST_F(DRMGPUProbeUnitTest, IntelVGA_OnBusZero_IsIntegrated)
{
    std::ignore = makeCardAt("card0", "0000:00:02.0", "i915");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_TRUE(gpus[0].isIntegrated);
}

// An iGPU that isn't the boot VGA device reports the Display-controller class, still on bus 0.
TEST_F(DRMGPUProbeUnitTest, IntelDisplayController_OnBusZero_IsIntegrated)
{
    const auto pciDir = makeCardAt("card0", "0000:00:02.0", "xe");
    writeFile(pciDir / "class", "0x038000");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_TRUE(gpus[0].isIntegrated);
}

// =============================================================================
// Per-adapter sensor capabilities (#1112) and memory availability (#1115)
// =============================================================================

// An i915 iGPU has no hwmon: no temperature, and nothing the DRM probe can't read (power, fan) is
// claimed for it, so the OR'd Linux capabilities don't draw NVML's series for it.
TEST_F(DRMGPUProbeUnitTest, SensorCapabilities_FollowTheCardsFiles)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");
    writeFile(m_SysRoot / "card0" / "gt_cur_freq_mhz", "1100");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    ASSERT_TRUE(gpus[0].sensorCapabilities.has_value());
    const auto sensors = gpus[0].sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_FALSE(sensors.hasTemperature);
    EXPECT_TRUE(sensors.hasClockSpeeds);
    EXPECT_FALSE(sensors.hasPowerMetrics);
    EXPECT_FALSE(sensors.hasFanSpeed);
    EXPECT_FALSE(sensors.hasHotspotTemp);
}

TEST_F(DRMGPUProbeUnitTest, SensorCapabilities_HwmonTemperatureIsReported)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");
    std::filesystem::create_directories(deviceDir / "hwmon" / "hwmon3");
    writeFile(deviceDir / "hwmon" / "hwmon3" / "temp1_input", "45000");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    ASSERT_TRUE(gpus[0].sensorCapabilities.has_value());
    EXPECT_TRUE(gpus[0].sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature);
    EXPECT_FALSE(gpus[0].sensorCapabilities.value_or(GPUCapabilities{}).hasClockSpeeds);
}

// An iGPU has no dedicated memory to read: its memory is unavailable, not a real-looking 0%.
TEST_F(DRMGPUProbeUnitTest, ReadGPUCounters_NoVramFiles_MemoryUnavailable)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_FALSE(counters[0].memoryAvailable);
    EXPECT_FALSE(counters[0].suspended);
}

// =============================================================================
// Runtime PM (#1117)
// =============================================================================

TEST_F(DRMGPUProbeUnitTest, ReadGPUCounters_RuntimeSuspendedCard_IsNotRead)
{
    const auto pciDir = makeCardAt("card1", "0000:03:00.0", "i915");
    std::filesystem::create_directories(pciDir / "hwmon" / "hwmon2");
    writeFile(pciDir / "hwmon" / "hwmon2" / "temp1_input", "50000");
    writeFile(m_SysRoot / "card1" / "gt_cur_freq_mhz", "300");
    std::filesystem::create_directories(pciDir / "power");
    writeFile(pciDir / "power" / "runtime_status", "suspended");

    DRMGPUProbe probe(m_SysRoot.string());
    auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_TRUE(counters[0].suspended);
    EXPECT_FALSE(counters[0].temperatureAvailable);
    EXPECT_FALSE(counters[0].gpuClockAvailable);
    EXPECT_EQ(counters[0].temperatureC, 0);

    writeFile(pciDir / "power" / "runtime_status", "active");
    counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_FALSE(counters[0].suspended);
    EXPECT_TRUE(counters[0].temperatureAvailable);
    EXPECT_EQ(counters[0].temperatureC, 50);
    EXPECT_EQ(counters[0].gpuClockMHz, 300U);
}

TEST_F(DRMGPUProbeUnitTest, ReadGPUCounters_SuspendedCardKeepsItsVramCapacity)
{
    // #1272 review: a sleeping dGPU isn't read, but its VRAM capacity is still known; reporting 0
    // would drop the header's capacity label and the Overview VRAM total.
    // The DRM probe's only VRAM source is mem_info_vram_total (xe reports VRAM via an ioctl, #1283).
    const auto pciDir = makeCardAt("card1", "0000:03:00.0", "xe");
    writeFile(pciDir / "mem_info_vram_total", "17179869184"); // 16 GiB
    std::filesystem::create_directories(pciDir / "power");
    writeFile(pciDir / "power" / "runtime_status", "active");

    DRMGPUProbe probe(m_SysRoot.string());
    auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].memoryTotalBytes, 17179869184ULL);

    writeFile(pciDir / "power" / "runtime_status", "suspended");
    counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_TRUE(counters[0].suspended);
    EXPECT_EQ(counters[0].memoryTotalBytes, 17179869184ULL);
    EXPECT_FALSE(counters[0].memoryAvailable); // used bytes aren't read while it sleeps
}

} // namespace
} // namespace Platform

#endif // defined(__linux__) && __has_include(<unistd.h>)
