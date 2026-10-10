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

#include "Domain/GPUModel.h"
#include "Platform/GPUTypes.h"
#include "Platform/Linux/DRMGPUProbe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <unistd.h>

#if __has_include(<drm/xe_drm.h>) && __has_include(<drm/i915_drm.h>)
#include <drm/i915_drm.h>
#include <drm/xe_drm.h>
#endif

namespace Platform
{

/// Builds a GPUInfo for a card from its sysfs device directory, as enumeration would. Discovery keeps
/// only Intel (i915/xe) cards, so this is how an AMD card's classification is reached (#1344). Defined
/// outside the anonymous namespace so it matches DRMGPUProbe's `friend struct DRMGPUProbeTestAccessor`.
struct DRMGPUProbeTestAccessor
{
    [[nodiscard]] static GPUInfo
    gpuInfoFor(const DRMGPUProbe& probe, const std::filesystem::path& deviceDir, const std::string& gpuId, const std::string& driver)
    {
        DRMGPUProbe::DRMCard card;
        card.cardPath = deviceDir.parent_path().string();
        card.devicePath = deviceDir.string();
        card.driver = driver;
        card.gpuId = gpuId;
        return probe.cardToGPUInfo(card);
    }

    /// How many client fdinfo files the probe has opened or tried to (#1356).
    [[nodiscard]] static std::uint64_t fdinfoReads(const DRMGPUProbe& probe)
    {
        return probe.m_FdinfoReads;
    }
};

namespace
{

// =============================================================================
// Integration Tests (real /sys/class/drm — skipped when not present)
// =============================================================================

TEST(DRMGPUProbeIntegrationTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ const DRMGPUProbe probe; });
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
    const DRMGPUProbe probe(m_SysRoot.string());
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

    const DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());

    const auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasTemperature);
    EXPECT_TRUE(caps.hasClockSpeeds);
    // Per-process and encode/decode metrics not supported via DRM sysfs
    EXPECT_FALSE(caps.hasPerProcessMetrics);
    EXPECT_FALSE(caps.hasPerProcessUtilization); // #1210
    EXPECT_FALSE(caps.hasEncoderDecoder);
}

TEST_F(DRMGPUProbeUnitTest, Capabilities_UnavailableProbe_ReportsNoSupport)
{
    const DRMGPUProbe probe((m_SysRoot / "nonexistent").string());
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
void makeHwmon(const std::filesystem::path& deviceDir, const std::string& hwmonName = "hwmon0")
{
    std::filesystem::create_directories(deviceDir / "hwmon" / hwmonName);
}

void writeHwmonFile(const std::filesystem::path& deviceDir,
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

// xe has no temp1_input: its channel 1 has a label only, and the package temperature is temp2_input,
// labelled "pkg" (VRAM temp3_input, "vram", on some parts) (#1314).
TEST_F(DRMGPUProbeUnitTest, XeCard_PackageTemperatureReadFromTheInputLabelledPkg)
{
    const auto deviceDir = makeCard("card0", "xe");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");
    makeHwmon(deviceDir);
    writeHwmonFile(deviceDir, "hwmon0", "temp1_label", "mctp");
    writeHwmonFile(deviceDir, "hwmon0", "temp2_label", "pkg");
    writeHwmonFile(deviceDir, "hwmon0", "temp2_input", "61000");
    writeHwmonFile(deviceDir, "hwmon0", "temp3_label", "vram");
    writeHwmonFile(deviceDir, "hwmon0", "temp3_input", "70000");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_TRUE(gpus[0].sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature);

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_TRUE(counters[0].temperatureAvailable);
    EXPECT_EQ(counters[0].temperatureC, 61) << "the package sensor, not VRAM";
}

TEST_F(DRMGPUProbeUnitTest, XeCard_OnlyTemp2InputLabelledPkgIsRead)
{
    // The issue's case: temp2_input + temp2_label = pkg, and nothing else.
    const auto deviceDir = makeCard("card0", "xe");
    writeFile(deviceDir / "vendor", "0x8086");
    makeHwmon(deviceDir);
    writeHwmonFile(deviceDir, "hwmon0", "temp2_input", "55000");
    writeHwmonFile(deviceDir, "hwmon0", "temp2_label", "pkg");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_TRUE(counters[0].temperatureAvailable);
    EXPECT_EQ(counters[0].temperatureC, 55);
}

TEST_F(DRMGPUProbeUnitTest, UnlabelledTemperatureInputsFallBackToTheLowestNumbered)
{
    const auto deviceDir = makeCard("card0", "xe");
    writeFile(deviceDir / "vendor", "0x8086");
    makeHwmon(deviceDir);
    writeHwmonFile(deviceDir, "hwmon0", "temp3_input", "70000");
    writeHwmonFile(deviceDir, "hwmon0", "temp2_input", "58000");
    writeHwmonFile(deviceDir, "hwmon0", "temp2_crit", "105000"); // not an input

    DRMGPUProbe probe(m_SysRoot.string());
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_TRUE(counters[0].temperatureAvailable);
    EXPECT_EQ(counters[0].temperatureC, 58);
}

TEST_F(DRMGPUProbeUnitTest, HwmonWithoutTemperatureInputHasNoTemperature)
{
    // An hwmon with only an energy counter (and xe's input-less channel-1 label) has no temperature sensor.
    const auto deviceDir = makeCard("card0", "xe");
    writeFile(deviceDir / "vendor", "0x8086");
    makeHwmon(deviceDir);
    writeHwmonFile(deviceDir, "hwmon0", "temp1_label", "pkg");
    writeHwmonFile(deviceDir, "hwmon0", "energy1_input", "1000");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_FALSE(gpus[0].sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature);
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
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
    // xe has no gt_cur_freq_mhz; its clock is under device/tile0/gt0/freq0 (#1268)
    std::filesystem::create_directories(dev1 / "tile0" / "gt0" / "freq0");
    writeFile(dev1 / "tile0" / "gt0" / "freq0" / "cur_freq", "950");
    writeFile(dev1 / "mem_info_vram_used", "1073741824");
    writeFile(dev1 / "mem_info_vram_total", "4294967296");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_TRUE(probe.isAvailable());
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 2U);

    // Find card0 counters (order may vary depending on directory iteration)
    const auto* c0 = counters[0].gpuClockMHz == 1200U ? counters.data() : std::next(counters.data());
    const auto* c1 = counters[0].gpuClockMHz == 1200U ? std::next(counters.data()) : counters.data();

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
// AMD APUs (#1344): the shared AmdApu rule, not the carve-out or the VGA class
// =============================================================================

constexpr uint64_t MIB = 1024ULL * 1024ULL;

/// An amdgpu PCI device directory with `vendor`, `class`, `device` and `mem_info_vram_total`, and an
/// ip_discovery GC entry when `gc` is given ({11, 0, 1} for GC 11.0.1).
[[nodiscard]] std::filesystem::path makeAmdDevice(const std::filesystem::path& root,
                                                  const std::string& pciAddress,
                                                  const std::string& deviceId,
                                                  uint64_t vramTotalBytes,
                                                  std::optional<std::tuple<int, int, int>> gc)
{
    const auto dir = root / "pci" / pciAddress;
    std::filesystem::create_directories(dir);
    const auto write = [](const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream file(path);
        file << text << '\n';
    };
    write(dir / "vendor", "0x1002");
    write(dir / "class", "0x030000"); // VGA compatible, as both APUs and Radeon cards are
    write(dir / "device", deviceId);
    write(dir / "mem_info_vram_total", std::to_string(vramTotalBytes));
    if (gc.has_value())
    {
        const auto gcDir = dir / "ip_discovery" / "die" / "0" / "GC" / "0";
        std::filesystem::create_directories(gcDir);
        write(gcDir / "major", std::to_string(std::get<0>(*gc)));
        write(gcDir / "minor", std::to_string(std::get<1>(*gc)));
        write(gcDir / "revision", std::to_string(std::get<2>(*gc)));
    }
    return dir;
}

// The bug: a 512 MiB carve-out in mem_info_vram_total read as dedicated VRAM, and any non-Intel VGA
// controller as discrete, so a Phoenix APU was "Discrete" and its carve-out joined the Overview's
// VRAM total (#1114, which counts discrete GPUs only).
TEST_F(DRMGPUProbeUnitTest, AmdApu_CarveOutAndApuGraphicsCoreVersion_IsIntegrated)
{
    const auto dir = makeAmdDevice(m_SysRoot, "0000:c4:00.0", "0x15bf", 512 * MIB, std::tuple{11, 0, 1}); // Phoenix

    const DRMGPUProbe probe(m_SysRoot.string());
    const auto info = DRMGPUProbeTestAccessor::gpuInfoFor(probe, dir, "0000:c4:00.0", "amdgpu");
    EXPECT_EQ(info.vendor, "AMD");
    EXPECT_TRUE(info.isIntegrated);
}

// A kernel without ip_discovery: the PCI device id decides, as in the ROCm probe.
TEST_F(DRMGPUProbeUnitTest, AmdApu_WithoutIpDiscovery_DeviceIdDecides)
{
    const auto renoir = makeAmdDevice(m_SysRoot, "0000:05:00.0", "0x1636", 2048 * MIB, std::nullopt);

    const DRMGPUProbe probe(m_SysRoot.string());
    EXPECT_TRUE(DRMGPUProbeTestAccessor::gpuInfoFor(probe, renoir, "0000:05:00.0", "amdgpu").isIntegrated);
}

TEST_F(DRMGPUProbeUnitTest, AmdDiscreteNavi_IsDiscrete)
{
    // Navi 21 (GC 10.3.0) with 16 GiB of real VRAM.
    const auto navi = makeAmdDevice(m_SysRoot, "0000:03:00.0", "0x73bf", 16ULL * 1024 * MIB, std::tuple{10, 3, 0});

    const DRMGPUProbe probe(m_SysRoot.string());
    EXPECT_FALSE(DRMGPUProbeTestAccessor::gpuInfoFor(probe, navi, "0000:03:00.0", "amdgpu").isIntegrated);
}

// A discrete GC version is discrete even with an APU-looking device id, and no signal at all is discrete.
TEST_F(DRMGPUProbeUnitTest, AmdWithDiscreteGraphicsCoreOrNoSignal_IsDiscrete)
{
    const auto discreteGc = makeAmdDevice(m_SysRoot, "0000:03:00.0", "0x15bf", 8ULL * 1024 * MIB, std::tuple{11, 0, 0});
    const auto noSignal = makeAmdDevice(m_SysRoot, "0000:04:00.0", "garbage", 8ULL * 1024 * MIB, std::nullopt);

    const DRMGPUProbe probe(m_SysRoot.string());
    EXPECT_FALSE(DRMGPUProbeTestAccessor::gpuInfoFor(probe, discreteGc, "0000:03:00.0", "amdgpu").isIntegrated);
    EXPECT_FALSE(DRMGPUProbeTestAccessor::gpuInfoFor(probe, noSignal, "0000:04:00.0", "amdgpu").isIntegrated);
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

// =============================================================================
// Restricted sysfs (#1165)
// =============================================================================

// A sandbox (Snap, Flatpak, AppArmor) can deny parts of /sys. The throwing std::filesystem
// overloads turned that into a filesystem_error escaping the constructor, aborting startup.
TEST_F(DRMGPUProbeUnitTest, UnreadableDeviceDirectory_ConstructorDoesNotThrow)
{
    if (getuid() == 0)
    {
        GTEST_SKIP() << "Cannot test EACCES as root";
    }

    const auto deviceDir = makeCard("card0", "i915");
    makeHwmon(deviceDir);
    std::filesystem::permissions(deviceDir, std::filesystem::perms::none);

    EXPECT_NO_THROW({
        const DRMGPUProbe probe(m_SysRoot.string());
        // The driver symlink can't be read, so the card isn't recognised as Intel.
        EXPECT_FALSE(probe.isAvailable());
    });

    std::filesystem::permissions(deviceDir, std::filesystem::perms::all);
}

TEST_F(DRMGPUProbeUnitTest, UnreadableHwmonDirectory_CardFoundWithoutHwmon)
{
    if (getuid() == 0)
    {
        GTEST_SKIP() << "Cannot test EACCES as root";
    }

    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    makeHwmon(deviceDir);
    writeHwmonFile(deviceDir, "hwmon0", "temp1_input", "50000");
    std::filesystem::permissions(deviceDir / "hwmon", std::filesystem::perms::none);

    EXPECT_NO_THROW({
        DRMGPUProbe probe(m_SysRoot.string());
        ASSERT_TRUE(probe.isAvailable());
        const auto counters = probe.readGPUCounters();
        ASSERT_EQ(counters.size(), 1U);
        EXPECT_FALSE(counters[0].temperatureAvailable);
    });

    std::filesystem::permissions(deviceDir / "hwmon", std::filesystem::perms::all);
}

// =============================================================================
// xe clock (#1268)
// =============================================================================

// xe has no gt_cur_freq_mhz; its frequency sysfs is <device>/tile#/gt#/freq0/ (xe_gt_freq.c).
TEST_F(DRMGPUProbeUnitTest, XeCard_ClockReadFromTileGtFreq)
{
    const auto pciDir = makeCardAt("card1", "0000:03:00.0", "xe");
    std::filesystem::create_directories(pciDir / "tile0" / "gt0" / "freq0");
    writeFile(pciDir / "tile0" / "gt0" / "freq0" / "cur_freq", "1850");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_TRUE(gpus[0].sensorCapabilities.value_or(GPUCapabilities{}).hasClockSpeeds);

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_TRUE(counters[0].gpuClockAvailable);
    EXPECT_EQ(counters[0].gpuClockMHz, 1850U);
}

TEST_F(DRMGPUProbeUnitTest, XeCard_NoFreqDirectory_ClockUnavailable)
{
    std::ignore = makeCardAt("card1", "0000:03:00.0", "xe");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_FALSE(gpus[0].sensorCapabilities.value_or(GPUCapabilities{}).hasClockSpeeds);
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_FALSE(counters[0].gpuClockAvailable);
}

// =============================================================================
// Power from the hwmon energy counter (#1269)
// =============================================================================

// Neither i915 nor xe exposes power1_input; the probe passes the µJ energy counter on and Domain
// derives watts from its change (GPUModel tests cover the derivation).
TEST_F(DRMGPUProbeUnitTest, EnergyCounter_IsReportedWithThePowerCapability)
{
    const auto pciDir = makeCardAt("card1", "0000:03:00.0", "i915");
    std::filesystem::create_directories(pciDir / "hwmon" / "hwmon4");
    const auto energyFile = pciDir / "hwmon" / "hwmon4" / "energy1_input";
    writeFile(energyFile, "1000000000"); // µJ

    DRMGPUProbe probe(m_SysRoot.string());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_TRUE(gpus[0].sensorCapabilities.value_or(GPUCapabilities{}).hasPowerMetrics);
    EXPECT_TRUE(probe.capabilities().hasPowerMetrics);

    auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_TRUE(counters[0].energyAvailable);
    EXPECT_EQ(counters[0].energyMicroJoules, 1000000000ULL);
    EXPECT_FALSE(counters[0].powerAvailable); // No instantaneous power: Domain derives it

    // A failed read is unavailable, not a counter of 0 (which would read as a reset).
    writeFile(energyFile, "garbage");
    counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_FALSE(counters[0].energyAvailable);
}

// xe on DG2/PVC registers only the package energy channel (energy2_input).
TEST_F(DRMGPUProbeUnitTest, EnergyCounter_XePackageChannelIsUsedWhenCardChannelIsAbsent)
{
    const auto pciDir = makeCardAt("card1", "0000:03:00.0", "xe");
    std::filesystem::create_directories(pciDir / "hwmon" / "hwmon5");
    writeFile(pciDir / "hwmon" / "hwmon5" / "energy2_input", "500000");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_TRUE(counters[0].energyAvailable);
    EXPECT_EQ(counters[0].energyMicroJoules, 500000ULL);
}

TEST_F(DRMGPUProbeUnitTest, EnergyCounter_NoHwmon_NoPowerCapability)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_FALSE(gpus[0].sensorCapabilities.value_or(GPUCapabilities{}).hasPowerMetrics);
    EXPECT_FALSE(probe.capabilities().hasPowerMetrics);
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_FALSE(counters[0].energyAvailable);
    EXPECT_FALSE(counters[0].powerAvailable);
}

// A suspended card's energy counter isn't read (an i915/xe hwmon read wakes it), so Domain's next
// awake sample has no previous counter to take a delta against.
TEST_F(DRMGPUProbeUnitTest, EnergyCounter_NotReadWhileSuspended)
{
    const auto pciDir = makeCardAt("card1", "0000:03:00.0", "i915");
    std::filesystem::create_directories(pciDir / "hwmon" / "hwmon4");
    writeFile(pciDir / "hwmon" / "hwmon4" / "energy1_input", "1000000");
    std::filesystem::create_directories(pciDir / "power");
    writeFile(pciDir / "power" / "runtime_status", "suspended");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_TRUE(counters[0].suspended);
    EXPECT_FALSE(counters[0].energyAvailable);
    EXPECT_FALSE(counters[0].powerAvailable);
}

// =============================================================================
// VRAM from the DRM memory-region query (#1283)
// =============================================================================

constexpr uint64_t GIB = 1024ULL * 1024ULL * 1024ULL;

/// A scripted VramQuery recording each call.
struct ScriptedVramQuery
{
    struct State
    {
        std::optional<DRMGPUProbe::VramInfo> reply;
        int calls = 0;
        std::string renderNode;
        std::string driver;
    };
    std::shared_ptr<State> state = std::make_shared<State>();

    [[nodiscard]] DRMGPUProbe::VramQuery fn() const
    {
        return [state = state](const std::string& renderNode, const std::string& driver)
        {
            ++state->calls;
            state->renderNode = renderNode;
            state->driver = driver;
            return state->reply;
        };
    }
};

TEST_F(DRMGPUProbeUnitTest, VramQuery_TotalIsReportedCachedAndKeptWhileSuspended)
{
    const auto pciDir = makeCardAt("card1", "0000:03:00.0", "xe");
    std::filesystem::create_directories(pciDir / "drm" / "card1");
    std::filesystem::create_directories(pciDir / "drm" / "renderD129");
    std::filesystem::create_directories(pciDir / "power");
    writeFile(pciDir / "power" / "runtime_status", "active");

    const ScriptedVramQuery query;
    query.state->reply = DRMGPUProbe::VramInfo{.totalBytes = 16 * GIB, .usedBytes = std::nullopt};
    DRMGPUProbe probe(m_SysRoot.string(), query.fn());

    auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].memoryTotalBytes, 16 * GIB);
    EXPECT_FALSE(counters[0].memoryAvailable); // No used figure: not a real-looking 0%
    EXPECT_EQ(query.state->renderNode, "/dev/dri/renderD129");
    EXPECT_EQ(query.state->driver, "xe");

    // The total doesn't change, so the render node isn't reopened every sample.
    counters = probe.readGPUCounters();
    EXPECT_EQ(counters[0].memoryTotalBytes, 16 * GIB);
    EXPECT_EQ(query.state->calls, 1);

    writeFile(pciDir / "power" / "runtime_status", "suspended");
    counters = probe.readGPUCounters();
    EXPECT_TRUE(counters[0].suspended);
    EXPECT_EQ(counters[0].memoryTotalBytes, 16 * GIB);
    EXPECT_EQ(query.state->calls, 1);
}

// The ioctl takes a runtime-PM reference, so it would wake a sleeping card: it waits until it's awake.
TEST_F(DRMGPUProbeUnitTest, VramQuery_NotIssuedWhileSuspended)
{
    const auto pciDir = makeCardAt("card1", "0000:03:00.0", "i915");
    std::filesystem::create_directories(pciDir / "drm" / "renderD128");
    std::filesystem::create_directories(pciDir / "power");
    writeFile(pciDir / "power" / "runtime_status", "suspended");

    const ScriptedVramQuery query;
    query.state->reply = DRMGPUProbe::VramInfo{.totalBytes = 8 * GIB, .usedBytes = 2 * GIB};
    DRMGPUProbe probe(m_SysRoot.string(), query.fn());

    std::ignore = probe.readGPUCounters();
    EXPECT_EQ(query.state->calls, 0);

    writeFile(pciDir / "power" / "runtime_status", "active");
    const auto counters = probe.readGPUCounters();
    EXPECT_EQ(query.state->calls, 1);
    EXPECT_EQ(query.state->driver, "i915");
    EXPECT_TRUE(counters[0].memoryAvailable);
    EXPECT_EQ(counters[0].memoryTotalBytes, 8 * GIB);
    EXPECT_EQ(counters[0].memoryUsedBytes, 2 * GIB);
}

// Where the kernel reports used memory, it is refreshed every awake sample.
TEST_F(DRMGPUProbeUnitTest, VramQuery_UsedIsRefreshedWhileReported)
{
    const auto pciDir = makeCardAt("card1", "0000:03:00.0", "xe");
    std::filesystem::create_directories(pciDir / "drm" / "renderD128");

    const ScriptedVramQuery query;
    query.state->reply = DRMGPUProbe::VramInfo{.totalBytes = 12 * GIB, .usedBytes = 1 * GIB};
    DRMGPUProbe probe(m_SysRoot.string(), query.fn());
    EXPECT_EQ(probe.readGPUCounters()[0].memoryUsedBytes, 1 * GIB);

    query.state->reply = DRMGPUProbe::VramInfo{.totalBytes = 12 * GIB, .usedBytes = 3 * GIB};
    const auto counters = probe.readGPUCounters();
    EXPECT_EQ(query.state->calls, 2);
    EXPECT_TRUE(counters[0].memoryAvailable);
    EXPECT_EQ(counters[0].memoryUsedBytes, 3 * GIB);
}

TEST_F(DRMGPUProbeUnitTest, VramQuery_FailureIsNotRetriedEverySample)
{
    const auto pciDir = makeCardAt("card1", "0000:03:00.0", "xe");
    std::filesystem::create_directories(pciDir / "drm" / "renderD128");

    const ScriptedVramQuery query; // reply = nullopt: the query fails
    DRMGPUProbe probe(m_SysRoot.string(), query.fn());
    auto counters = probe.readGPUCounters();
    EXPECT_EQ(counters[0].memoryTotalBytes, 0ULL);
    EXPECT_FALSE(counters[0].memoryAvailable);
    counters = probe.readGPUCounters();
    EXPECT_EQ(query.state->calls, 1);
}

TEST_F(DRMGPUProbeUnitTest, VramQuery_NoRenderNode_NotQueried)
{
    std::ignore = makeCardAt("card1", "0000:03:00.0", "xe");

    const ScriptedVramQuery query;
    query.state->reply = DRMGPUProbe::VramInfo{.totalBytes = 16 * GIB, .usedBytes = std::nullopt};
    DRMGPUProbe probe(m_SysRoot.string(), query.fn());
    EXPECT_EQ(probe.readGPUCounters()[0].memoryTotalBytes, 0ULL);
    EXPECT_EQ(query.state->calls, 0);
}

// A queried VRAM total is evidence of a discrete GPU, as mem_info_vram_total is.
TEST_F(DRMGPUProbeUnitTest, VramQuery_TotalClassifiesTheCardAsDiscrete)
{
    const auto deviceDir = makeCard("card0", "xe"); // No PCI address: no bus to classify by
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");
    std::filesystem::create_directories(deviceDir / "drm" / "renderD128");

    const ScriptedVramQuery query;
    query.state->reply = DRMGPUProbe::VramInfo{.totalBytes = 4 * GIB, .usedBytes = std::nullopt};
    DRMGPUProbe probe(m_SysRoot.string(), query.fn());
    EXPECT_TRUE(probe.enumerateGPUs()[0].isIntegrated); // Not queried yet
    std::ignore = probe.readGPUCounters();
    EXPECT_FALSE(probe.enumerateGPUs()[0].isIntegrated);
}

// GPUModel enumerates once and re-enumerates only when rescanGPUs() says to, so a total the query
// learns after enumeration is reported by the next rescan -- a quick one, every sample -- and only once.
TEST_F(DRMGPUProbeUnitTest, VramQuery_NewTotalAsksForReEnumerationOnce)
{
    const auto deviceDir = makeCard("card0", "xe"); // No PCI address: classified by VRAM
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");
    std::filesystem::create_directories(deviceDir / "drm" / "renderD128");

    const ScriptedVramQuery query;
    query.state->reply = DRMGPUProbe::VramInfo{.totalBytes = 4 * GIB, .usedBytes = 1 * GIB};
    DRMGPUProbe probe(m_SysRoot.string(), query.fn());
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));

    std::ignore = probe.readGPUCounters();
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_FALSE(probe.enumerateGPUs()[0].isIntegrated);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)); // Reported once

    // Used is re-queried each sample; the same total is no change.
    query.state->reply = DRMGPUProbe::VramInfo{.totalBytes = 4 * GIB, .usedBytes = 2 * GIB};
    std::ignore = probe.readGPUCounters();
    EXPECT_EQ(query.state->calls, 2);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));

    // A full rescan with no card change reports it too.
    query.state->reply = DRMGPUProbe::VramInfo{.totalBytes = 8 * GIB, .usedBytes = 2 * GIB};
    std::ignore = probe.readGPUCounters();
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
}

// #1321 review: through GPUModel, which enumerated before the first counter read, the queried total
// reaches the published GPUInfo and snapshots: the card becomes discrete, with its VRAM capacity.
TEST_F(DRMGPUProbeUnitTest, VramQuery_GPUModelPublishesTheCardAsDiscreteWithTheQueriedTotal)
{
    const auto deviceDir = makeCard("card0", "xe"); // No PCI address: classified by VRAM
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");
    std::filesystem::create_directories(deviceDir / "drm" / "renderD128");

    const ScriptedVramQuery query;
    query.state->reply = DRMGPUProbe::VramInfo{.totalBytes = 16 * GIB, .usedBytes = 3 * GIB};
    Domain::GPUModel model(std::make_unique<DRMGPUProbe>(m_SysRoot.string(), query.fn()));
    ASSERT_EQ(model.gpuInfo().size(), 1U);
    EXPECT_TRUE(model.gpuInfo()[0].isIntegrated); // Enumerated before any query

    const auto start = std::chrono::steady_clock::now();
    model.refreshAt(start); // The first awake sample issues the query
    model.refreshAt(start + std::chrono::seconds(1));

    const auto info = model.gpuInfo();
    ASSERT_EQ(info.size(), 1U);
    EXPECT_FALSE(info[0].isIntegrated);
    const auto snapshots = model.snapshots();
    ASSERT_EQ(snapshots.size(), 1U);
    EXPECT_FALSE(snapshots[0].isIntegrated);
    EXPECT_EQ(snapshots[0].memoryTotalBytes, 16 * GIB);
    EXPECT_EQ(snapshots[0].memoryUsedBytes, 3 * GIB);
}

#if __has_include(<drm/xe_drm.h>) && __has_include(<drm/i915_drm.h>)

/// Lays out a DRM query reply: a header of `headerBytes` whose first __u32 is the region count,
/// followed by the regions, in an 8-byte-aligned buffer as the kernel would fill it.
template<typename Region> class QueryReply
{
  public:
    QueryReply(std::size_t headerBytes, const std::vector<Region>& regions)
        : m_Bytes(headerBytes + (regions.size() * sizeof(Region))), m_Storage((m_Bytes + 7) / 8, 0)
    {
        const auto count = static_cast<uint32_t>(regions.size());
        auto bytes = std::as_writable_bytes(std::span(m_Storage));
        std::memcpy(bytes.data(), &count, sizeof(count));
        for (std::size_t i = 0; i < regions.size(); ++i)
        {
            std::memcpy(bytes.subspan(headerBytes + (i * sizeof(Region))).data(), &regions[i], sizeof(Region));
        }
    }

    [[nodiscard]] std::span<const std::byte> bytes(std::size_t trim = 0) const
    {
        return std::as_bytes(std::span(m_Storage)).first(m_Bytes - trim);
    }

  private:
    std::size_t m_Bytes;
    std::vector<uint64_t> m_Storage;
};

TEST(DRMGPUProbeQueryReplyTest, XeMemRegions_SumsVramRegionsOnly)
{
    drm_xe_mem_region sysmem{};
    sysmem.mem_class = DRM_XE_MEM_REGION_CLASS_SYSMEM;
    sysmem.total_size = 32 * GIB;
    sysmem.used = 5 * GIB;
    drm_xe_mem_region vram0{};
    vram0.mem_class = DRM_XE_MEM_REGION_CLASS_VRAM;
    vram0.total_size = 8 * GIB;
    vram0.used = 1 * GIB;
    drm_xe_mem_region vram1 = vram0;
    vram1.instance = 2;

    const QueryReply reply(offsetof(drm_xe_query_mem_regions, mem_regions), std::vector{sysmem, vram0, vram1});
    const auto info = DRMGPUProbe::summarizeXeMemRegions(reply.bytes());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info.value_or(DRMGPUProbe::VramInfo{}).totalBytes, 16 * GIB);
    EXPECT_EQ(info.value_or(DRMGPUProbe::VramInfo{}).usedBytes, 2 * GIB);

    // A reply too short for the regions it claims is rejected, not read past its end.
    EXPECT_FALSE(DRMGPUProbe::summarizeXeMemRegions(reply.bytes(1)).has_value());
}

TEST(DRMGPUProbeQueryReplyTest, XeMemRegions_IntegratedHasNoVramAndZeroUsedIsUnreported)
{
    drm_xe_mem_region sysmem{};
    sysmem.mem_class = DRM_XE_MEM_REGION_CLASS_SYSMEM;
    sysmem.total_size = 16 * GIB;
    const QueryReply igpu(offsetof(drm_xe_query_mem_regions, mem_regions), std::vector{sysmem});
    const auto igpuInfo = DRMGPUProbe::summarizeXeMemRegions(igpu.bytes());
    ASSERT_TRUE(igpuInfo.has_value());
    EXPECT_EQ(igpuInfo.value_or(DRMGPUProbe::VramInfo{}).totalBytes, 0ULL);
    EXPECT_FALSE(igpuInfo.value_or(DRMGPUProbe::VramInfo{}).usedBytes.has_value());

    drm_xe_mem_region vram{};
    vram.mem_class = DRM_XE_MEM_REGION_CLASS_VRAM;
    vram.total_size = 12 * GIB; // used = 0: an older kernel without CAP_PERFMON
    const QueryReply dgpu(offsetof(drm_xe_query_mem_regions, mem_regions), std::vector{vram});
    const auto dgpuInfo = DRMGPUProbe::summarizeXeMemRegions(dgpu.bytes());
    ASSERT_TRUE(dgpuInfo.has_value());
    EXPECT_EQ(dgpuInfo.value_or(DRMGPUProbe::VramInfo{}).totalBytes, 12 * GIB);
    EXPECT_FALSE(dgpuInfo.value_or(DRMGPUProbe::VramInfo{}).usedBytes.has_value());
}

TEST(DRMGPUProbeQueryReplyTest, I915MemRegions_UsedOnlyWhenTheKernelAccountsForIt)
{
    drm_i915_memory_region_info sysmem{};
    sysmem.region.memory_class = I915_MEMORY_CLASS_SYSTEM;
    sysmem.probed_size = 32 * GIB;
    sysmem.unallocated_size = 30 * GIB;
    drm_i915_memory_region_info lmem{};
    lmem.region.memory_class = I915_MEMORY_CLASS_DEVICE;
    lmem.probed_size = 8 * GIB;
    lmem.unallocated_size = 8 * GIB; // Without CAP_PERFMON: unallocated == probed

    const std::size_t header = offsetof(drm_i915_query_memory_regions, regions);
    const QueryReply unprivileged(header, std::vector{sysmem, lmem});
    const auto info = DRMGPUProbe::summarizeI915MemRegions(unprivileged.bytes());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info.value_or(DRMGPUProbe::VramInfo{}).totalBytes, 8 * GIB);
    EXPECT_FALSE(info.value_or(DRMGPUProbe::VramInfo{}).usedBytes.has_value());

    lmem.unallocated_size = 6 * GIB;
    const QueryReply privileged(header, std::vector{sysmem, lmem});
    const auto privilegedInfo = DRMGPUProbe::summarizeI915MemRegions(privileged.bytes());
    ASSERT_TRUE(privilegedInfo.has_value());
    EXPECT_EQ(privilegedInfo.value_or(DRMGPUProbe::VramInfo{}).usedBytes, 2 * GIB);

    EXPECT_FALSE(DRMGPUProbe::summarizeI915MemRegions(privileged.bytes(8)).has_value());
}

#endif // __has_include(<drm/xe_drm.h>) && __has_include(<drm/i915_drm.h>)

// =============================================================================
// Re-enumeration (#1116)
// =============================================================================

// A card that appears after startup (an Intel eGPU, a driver bound late) is
// found by the next full rescan; a quick rescan doesn't look, and an unchanged
// tree reports no change.
TEST_F(DRMGPUProbeUnitTest, RescanGPUs_FullRescanFindsACardAddedAfterConstruction)
{
    std::ignore = makeCardAt("card0", "0000:00:02.0");
    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_EQ(probe.enumerateGPUs().size(), 1U);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));

    const auto pciDir = makeCardAt("card1", "0000:03:00.0", "xe");
    writeFile(pciDir / "mem_info_vram_total", "17179869184");
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(gpus[0].id, "0000:00:02.0");
    EXPECT_EQ(gpus[1].id, "0000:03:00.0");
    EXPECT_EQ(probe.readGPUCounters().size(), 2U);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));
}

// A card that goes away is dropped by the next full rescan; the survivor keeps
// its id and its last-known VRAM capacity.
TEST_F(DRMGPUProbeUnitTest, RescanGPUs_FullRescanDropsARemovedCard)
{
    std::ignore = makeCardAt("card0", "0000:00:02.0");
    const auto dgpu = makeCardAt("card1", "0000:03:00.0", "xe");
    writeFile(dgpu / "mem_info_vram_total", "17179869184");
    std::filesystem::create_directories(dgpu / "power");
    writeFile(dgpu / "power" / "runtime_status", "active");

    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_EQ(probe.readGPUCounters().size(),
              2U); // records the dGPU's VRAM total while awake

    std::filesystem::remove_all(m_SysRoot / "card0");
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].id, "0000:03:00.0");

    writeFile(dgpu / "power" / "runtime_status", "suspended");
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_TRUE(counters[0].suspended);
    EXPECT_EQ(counters[0].memoryTotalBytes, 17179869184ULL);
}

// A probe that found no Intel card at startup becomes available when one
// appears.
TEST_F(DRMGPUProbeUnitTest, RescanGPUs_ProbeWithNoCardBecomesAvailable)
{
    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_FALSE(probe.isAvailable());

    std::ignore = makeCardAt("card0", "0000:00:02.0");
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_TRUE(probe.isAvailable());
    EXPECT_EQ(probe.enumerateGPUs().size(), 1U);
}

// A card whose hwmon registers after the driver bound gets its temperature
// sensor on the next full rescan (DRM's per-card sensor set comes from which
// files the card has, #1112).
TEST_F(DRMGPUProbeUnitTest, RescanGPUs_LateHwmonIsPickedUp)
{
    const auto pciDir = makeCardAt("card1", "0000:03:00.0", "xe");
    DRMGPUProbe probe(m_SysRoot.string());
    ASSERT_EQ(probe.enumerateGPUs().size(), 1U);
    EXPECT_FALSE(probe.enumerateGPUs()[0].sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature);

    std::filesystem::create_directories(pciDir / "hwmon" / "hwmon3");
    writeFile(pciDir / "hwmon" / "hwmon3" / "temp1_input", "48000");
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_TRUE(gpus[0].sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature);
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].temperatureC, 48);
}

// A rescan that rebuilds the card list (here for a hot-plugged iGPU) keeps an unchanged card's DRM
// query results (#1283): its VRAM total isn't re-queried, and is still known while it sleeps.
TEST_F(DRMGPUProbeUnitTest, RescanGPUs_UnchangedCardKeepsItsQueriedVramWithoutReQuerying)
{
    const auto pciDir = makeCardAt("card1", "0000:03:00.0", "xe");
    std::filesystem::create_directories(pciDir / "drm" / "renderD129");
    std::filesystem::create_directories(pciDir / "power");
    writeFile(pciDir / "power" / "runtime_status", "active");

    const ScriptedVramQuery query;
    query.state->reply = DRMGPUProbe::VramInfo{.totalBytes = 16 * GIB, .usedBytes = std::nullopt};
    DRMGPUProbe probe(m_SysRoot.string(), query.fn());
    ASSERT_EQ(probe.readGPUCounters()[0].memoryTotalBytes, 16 * GIB);
    ASSERT_EQ(query.state->calls, 1);

    std::ignore = makeCardAt("card0", "0000:00:02.0"); // No render node: never queried
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(gpus[1].id, "0000:03:00.0");
    EXPECT_FALSE(gpus[1].isIntegrated); // Still classified by the cached queried total

    auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 2U);
    EXPECT_EQ(counters[1].memoryTotalBytes, 16 * GIB);
    EXPECT_EQ(query.state->calls, 1);

    writeFile(pciDir / "power" / "runtime_status", "suspended");
    counters = probe.readGPUCounters();
    EXPECT_TRUE(counters[1].suspended);
    EXPECT_EQ(counters[1].memoryTotalBytes, 16 * GIB);
    EXPECT_EQ(query.state->calls, 1);
}

// A render node that registers after the card is a change the next full rescan picks up, so the
// card gets the VRAM query it couldn't have before (#1283).
TEST_F(DRMGPUProbeUnitTest, RescanGPUs_LateRenderNodeIsPickedUpAndQueried)
{
    const auto pciDir = makeCardAt("card1", "0000:03:00.0", "xe");
    const ScriptedVramQuery query;
    query.state->reply = DRMGPUProbe::VramInfo{.totalBytes = 8 * GIB, .usedBytes = std::nullopt};
    DRMGPUProbe probe(m_SysRoot.string(), query.fn());
    EXPECT_EQ(probe.readGPUCounters()[0].memoryTotalBytes, 0ULL);
    EXPECT_EQ(query.state->calls, 0);

    std::filesystem::create_directories(pciDir / "drm" / "renderD128");
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(probe.readGPUCounters()[0].memoryTotalBytes, 8 * GIB);
    EXPECT_EQ(query.state->calls, 1);
    EXPECT_EQ(query.state->renderNode, "/dev/dri/renderD128");
}

// #1321 review: a VRAM total remembered from the query belongs to the query target. If the card's
// render node or driver changes while it sleeps, the old target's capacity isn't reported for it;
// a sysfs total (mem_info_vram_total) isn't tied to the render node and is kept.
TEST_F(DRMGPUProbeUnitTest, RescanGPUs_QueriedTotalIsDroppedWhenTheQueryTargetChanges)
{
    const auto queried = makeCardAt("card1", "0000:03:00.0", "xe");
    std::filesystem::create_directories(queried / "drm" / "renderD129");
    std::filesystem::create_directories(queried / "power");
    writeFile(queried / "power" / "runtime_status", "active");
    const auto sysfs = makeCardAt("card2", "0000:04:00.0", "xe");
    writeFile(sysfs / "mem_info_vram_total", "8589934592"); // 8 GiB
    std::filesystem::create_directories(sysfs / "drm" / "renderD130");
    std::filesystem::create_directories(sysfs / "power");
    writeFile(sysfs / "power" / "runtime_status", "active");

    const ScriptedVramQuery query;
    query.state->reply = DRMGPUProbe::VramInfo{.totalBytes = 16 * GIB, .usedBytes = std::nullopt};
    DRMGPUProbe probe(m_SysRoot.string(), query.fn());
    auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 2U);
    ASSERT_EQ(counters[0].memoryTotalBytes, 16 * GIB);
    ASSERT_EQ(counters[1].memoryTotalBytes, 8 * GIB);

    // Both cards sleep, and their render nodes are renumbered (a driver rebind).
    writeFile(queried / "power" / "runtime_status", "suspended");
    writeFile(sysfs / "power" / "runtime_status", "suspended");
    std::filesystem::remove(queried / "drm" / "renderD129");
    std::filesystem::create_directories(queried / "drm" / "renderD131");
    std::filesystem::remove(sysfs / "drm" / "renderD130");
    std::filesystem::create_directories(sysfs / "drm" / "renderD132");
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));

    counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 2U);
    EXPECT_TRUE(counters[0].suspended);
    EXPECT_EQ(counters[0].memoryTotalBytes, 0ULL); // Unknown until the new target is queried
    EXPECT_TRUE(counters[1].suspended);
    EXPECT_EQ(counters[1].memoryTotalBytes, 8 * GIB);
    EXPECT_EQ(query.state->calls, 1);

    // Awake again, the new target is queried.
    query.state->reply = DRMGPUProbe::VramInfo{.totalBytes = 12 * GIB, .usedBytes = std::nullopt};
    writeFile(queried / "power" / "runtime_status", "active");
    counters = probe.readGPUCounters();
    EXPECT_EQ(counters[0].memoryTotalBytes, 12 * GIB);
    EXPECT_EQ(query.state->renderNode, "/dev/dri/renderD131");
}

// =============================================================================
// Utilization from the DRM clients' fdinfo engine busyness (#1267)
// =============================================================================

constexpr auto RENDER = static_cast<std::size_t>(GPUEngineClass::Render);
constexpr auto COPY = static_cast<std::size_t>(GPUEngineClass::Copy);
constexpr auto VIDEO = static_cast<std::size_t>(GPUEngineClass::Video);
constexpr auto COMPUTE = static_cast<std::size_t>(GPUEngineClass::Compute);

TEST(DRMGPUProbeFdinfoTest, I915BusyNanosecondsAreStampedWithTheClock)
{
    const auto info = DRMGPUProbe::parseFdinfo("pos:\t0\nflags:\t02100002\ndrm-driver:\ti915\ndrm-pdev:\t0000:00:02.0\n"
                                               "drm-client-id:\t7\ndrm-engine-render:\t5000 ns\ndrm-engine-copy:\t0 ns\n"
                                               "drm-engine-video:\t10 ns\ndrm-engine-capacity-video:\t2\n",
                                               123'456);
    ASSERT_TRUE(info.has_value());
    const auto parsed = info.value_or(DRMGPUProbe::DrmFdinfo{});
    EXPECT_EQ(parsed.client.clientId, 7U);
    EXPECT_EQ(parsed.pdev, "0000:00:02.0");
    EXPECT_TRUE(parsed.hasEngineStats);
    const auto& render = parsed.client.engines.at(RENDER);
    EXPECT_TRUE(render.available);
    EXPECT_EQ(render.busy, 5000U);
    EXPECT_EQ(render.total, 123'456U);
    EXPECT_EQ(render.capacity, 1U);
    EXPECT_TRUE(parsed.client.engines.at(COPY).available); // 0 ns is a reading, not a missing one
    EXPECT_EQ(parsed.client.engines.at(VIDEO).capacity, 2U);
    EXPECT_FALSE(parsed.client.engines.at(COMPUTE).available);
}

TEST(DRMGPUProbeFdinfoTest, XeCyclesPairWithTheirTotalCycles)
{
    const auto info = DRMGPUProbe::parseFdinfo("drm-driver:\txe\ndrm-client-id:\t42\n"
                                               "drm-cycles-rcs:\t100\ndrm-total-cycles-rcs:\t1000\n"
                                               "drm-cycles-vcs:\t3\ndrm-total-cycles-vcs:\t1000\ndrm-engine-capacity-vcs:\t2\n"
                                               "drm-cycles-ccs:\t5\n",
                                               999);
    ASSERT_TRUE(info.has_value());
    const auto parsed = info.value_or(DRMGPUProbe::DrmFdinfo{});
    EXPECT_EQ(parsed.client.clientId, 42U);
    const auto& render = parsed.client.engines.at(RENDER);
    EXPECT_TRUE(render.available);
    EXPECT_EQ(render.busy, 100U);
    EXPECT_EQ(render.total, 1000U); // The GPU timestamp, not the clock
    EXPECT_EQ(parsed.client.engines.at(VIDEO).capacity, 2U);
    EXPECT_FALSE(parsed.client.engines.at(COMPUTE).available); // Cycles without their total
}

TEST(DRMGPUProbeFdinfoTest, NotADrmFileOrNoEngineStats)
{
    // A regular file's fdinfo has no drm-client-id.
    EXPECT_FALSE(DRMGPUProbe::parseFdinfo("pos:\t0\nflags:\t0100000\nmnt_id:\t25\nino:\t1234\n", 1).has_value());
    // A DRM file on a kernel without fdinfo engine stats, and malformed values, carry none.
    const auto info = DRMGPUProbe::parseFdinfo("drm-driver:\ti915\ndrm-client-id:\t3\ndrm-engine-render:\tlots\n"
                                               "drm-engine-teleporter:\t5 ns\ndrm-engine-copy:\t12x ns\n",
                                               1);
    ASSERT_TRUE(info.has_value());
    EXPECT_FALSE(info.value_or(DRMGPUProbe::DrmFdinfo{}).hasEngineStats);
}

/// A process `pid` in the fake /proc under `procRoot` with fd `fd` linked to `target` and, if
/// given, that fd's fdinfo text.
void makeFd(const std::filesystem::path& procRoot,
            int pid,
            int fd,
            const std::string& target,
            const std::optional<std::string>& fdinfo = std::nullopt)
{
    const auto processDir = procRoot / std::to_string(pid);
    std::filesystem::create_directories(processDir / "fd");
    std::filesystem::create_directories(processDir / "fdinfo");
    std::filesystem::create_symlink(target, processDir / "fd" / std::to_string(fd));
    if (fdinfo.has_value())
    {
        std::ofstream(processDir / "fdinfo" / std::to_string(fd)) << *fdinfo;
    }
}

/// xe fdinfo text for client `clientId` on 0000:03:00.0 with these render-class cycles.
[[nodiscard]] std::string xeFdinfo(int clientId, std::uint64_t cycles, std::uint64_t totalCycles)
{
    return std::format("drm-driver:\txe\ndrm-pdev:\t0000:03:00.0\ndrm-client-id:\t{}\ndrm-cycles-rcs:\t{}\ndrm-total-cycles-rcs:\t{}\n",
                       clientId,
                       cycles,
                       totalCycles);
}

class DRMGPUProbeEngineTest : public DRMGPUProbeUnitTest
{
  protected:
    std::filesystem::path m_ProcRoot;
    std::filesystem::path m_PciDir;

    void SetUp() override
    {
        DRMGPUProbeUnitTest::SetUp();
        m_ProcRoot = m_SysRoot / "proc";
        std::filesystem::create_directories(m_ProcRoot);
        // An Arc on xe: card1, render node renderD129, awake.
        m_PciDir = makeCardAt("card1", "0000:03:00.0", "xe");
        std::filesystem::create_directories(m_PciDir / "drm" / "renderD129");
        std::filesystem::create_directories(m_PciDir / "power");
        writeFile(m_PciDir / "power" / "runtime_status", "active");
    }

    [[nodiscard]] std::unique_ptr<DRMGPUProbe> makeProbe(DRMGPUProbe::MonotonicClock clock = {}) const
    {
        const ScriptedVramQuery query; // No VRAM reply: keeps the real ioctl out of these tests
        return std::make_unique<DRMGPUProbe>(m_SysRoot.string(), query.fn(), m_ProcRoot.string(), std::move(clock));
    }
};

TEST_F(DRMGPUProbeEngineTest, ClientsOfTheCardAreReadOnceEach)
{
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", xeFdinfo(7, 10, 100));
    makeFd(m_ProcRoot, 100, 5, "/dev/dri/renderD129", xeFdinfo(7, 10, 100)); // dup'd: the same client
    makeFd(m_ProcRoot, 200, 3, "/dev/dri/card1", xeFdinfo(9, 20, 100));      // the card node counts too
    makeFd(m_ProcRoot, 300, 3, "/dev/null", "pos:\t0\n");
    makeFd(m_ProcRoot, 400, 3, "/dev/dri/renderD200", xeFdinfo(11, 30, 100)); // another device
    makeFd(m_ProcRoot,
           500,
           3,
           "/dev/dri/renderD129", // fd reused for another device's file
           "drm-driver:\txe\ndrm-pdev:\t0000:04:00.0\ndrm-client-id:\t12\ndrm-cycles-rcs:\t1\ndrm-total-cycles-rcs:\t2\n");

    const auto probe = makeProbe();
    const auto counters = probe->readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_TRUE(counters[0].engineBusyAvailable);
    EXPECT_FALSE(counters[0].utilizationAvailable); // Domain derives it; the probe never reads a percent
    ASSERT_EQ(counters[0].engineClients.size(), 2U);
    std::vector<std::uint64_t> ids{counters[0].engineClients[0].clientId, counters[0].engineClients[1].clientId};
    std::ranges::sort(ids);
    EXPECT_EQ(ids, (std::vector<std::uint64_t>{7, 9}));
}

// The /proc walk is the expensive part, so it runs at the first read and at full rescans only: the
// samples between re-read just the fdinfo files already found.
TEST_F(DRMGPUProbeEngineTest, NewClientsAreFoundAtFullRescansNotEverySample)
{
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", xeFdinfo(7, 10, 100));
    const auto probe = makeProbe();
    ASSERT_EQ(probe->readGPUCounters()[0].engineClients.size(), 1U);

    makeFd(m_ProcRoot, 200, 4, "/dev/dri/renderD129", xeFdinfo(8, 10, 100));
    EXPECT_EQ(probe->readGPUCounters()[0].engineClients.size(), 1U);
    std::ignore = probe->rescanGPUs(GPURescan::Quick);
    EXPECT_EQ(probe->readGPUCounters()[0].engineClients.size(), 1U);

    EXPECT_FALSE(probe->rescanGPUs(GPURescan::Full)); // Same cards: no re-enumeration
    EXPECT_EQ(probe->readGPUCounters()[0].engineClients.size(), 2U);
}

TEST_F(DRMGPUProbeEngineTest, AClosedClientIsDroppedAndNoClientsIsIdle)
{
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", xeFdinfo(7, 10, 100));
    const auto probe = makeProbe();
    ASSERT_EQ(probe->readGPUCounters()[0].engineClients.size(), 1U);

    std::filesystem::remove_all(m_ProcRoot / "100"); // The process exited
    const auto counters = probe->readGPUCounters();
    EXPECT_TRUE(counters[0].engineClients.empty());
    EXPECT_TRUE(counters[0].engineBusyAvailable); // Idle, not unread
}

// Whether a GPUModel fed by a probe over the fake /proc publishes a utilization after its second
// sample, and the percent if it does: 0% for an idle card, nothing (N/A) for an unknown one.
[[nodiscard]] std::optional<double> utilizationAfterTwoSamples(std::unique_ptr<DRMGPUProbe> probe)
{
    Domain::GPUModel model(std::move(probe));
    const auto start = std::chrono::steady_clock::now();
    model.refreshAt(start);
    model.refreshAt(start + std::chrono::seconds(1));
    const auto snapshots = model.snapshots();
    if (snapshots.size() != 1U || !snapshots[0].utilizationAvailable)
    {
        return std::nullopt;
    }
    return snapshots[0].utilizationPercent;
}

// A /proc the walk could read, with processes but none on the card: the card is idle, 0%.
TEST_F(DRMGPUProbeEngineTest, ReadableProcWithNoClientsIsIdle)
{
    makeFd(m_ProcRoot, 100, 3, "/dev/null", "pos:\t0\n");
    EXPECT_TRUE(makeProbe()->readGPUCounters()[0].engineBusyAvailable);
    const auto utilization = utilizationAfterTwoSamples(makeProbe());
    EXPECT_TRUE(utilization.has_value());
    EXPECT_DOUBLE_EQ(utilization.value_or(-1.0), 0.0);
}

// No /proc to walk: finding no clients there says nothing about the card, so N/A, not 0%.
TEST_F(DRMGPUProbeEngineTest, MissingProcRootLeavesBusynessUnread)
{
    std::filesystem::remove_all(m_ProcRoot);
    EXPECT_FALSE(makeProbe()->readGPUCounters()[0].engineBusyAvailable);
    EXPECT_FALSE(utilizationAfterTwoSamples(makeProbe()).has_value());
}

TEST_F(DRMGPUProbeEngineTest, UnreadableProcRootLeavesBusynessUnread)
{
    if (getuid() == 0)
    {
        GTEST_SKIP() << "Cannot test EACCES as root";
    }
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", xeFdinfo(7, 10, 100));
    std::filesystem::permissions(m_ProcRoot, std::filesystem::perms::none);
    const bool available = makeProbe()->readGPUCounters()[0].engineBusyAvailable;
    const auto utilization = utilizationAfterTwoSamples(makeProbe());
    std::filesystem::permissions(m_ProcRoot, std::filesystem::perms::all); // So TearDown can remove it
    EXPECT_FALSE(available);
    EXPECT_FALSE(utilization.has_value());
}

// A sandbox that lists /proc but denies every process's fd directory: unknown, not idle.
TEST_F(DRMGPUProbeEngineTest, EveryFdDirectoryDeniedLeavesBusynessUnread)
{
    if (getuid() == 0)
    {
        GTEST_SKIP() << "Cannot test EACCES as root";
    }
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", xeFdinfo(7, 10, 100));
    makeFd(m_ProcRoot, 200, 3, "/dev/null", "pos:\t0\n");
    for (const int pid : {100, 200})
    {
        std::filesystem::permissions(m_ProcRoot / std::to_string(pid) / "fd", std::filesystem::perms::none);
    }
    const bool available = makeProbe()->readGPUCounters()[0].engineBusyAvailable;
    const auto utilization = utilizationAfterTwoSamples(makeProbe());
    for (const int pid : {100, 200})
    {
        std::filesystem::permissions(m_ProcRoot / std::to_string(pid) / "fd", std::filesystem::perms::all);
    }
    EXPECT_FALSE(available);
    EXPECT_FALSE(utilization.has_value());
}

// xe takes a runtime-PM reference to report a client's cycles, so a sleeping card's fdinfo isn't read.
TEST_F(DRMGPUProbeEngineTest, ASuspendedCardsClientsAreNotRead)
{
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", xeFdinfo(7, 10, 100));
    writeFile(m_PciDir / "power" / "runtime_status", "suspended");
    const auto probe = makeProbe();
    const auto counters = probe->readGPUCounters();
    EXPECT_TRUE(counters[0].suspended);
    EXPECT_FALSE(counters[0].engineBusyAvailable);
    EXPECT_TRUE(counters[0].engineClients.empty());
}

// A client whose fdinfo has a drm-client-id but no engine busyness (none of its classes reported):
// utilization stays N/A, not 0%. A real pre-5.19 i915 prints no drm-* keys at all: see the next test.
TEST_F(DRMGPUProbeEngineTest, ClientsWithoutEngineStatsLeaveBusynessUnread)
{
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", "drm-driver:\txe\ndrm-pdev:\t0000:03:00.0\ndrm-client-id:\t7\n");
    const auto probe = makeProbe();
    EXPECT_FALSE(probe->readGPUCounters()[0].engineBusyAvailable);
}

/// A DRM fd's fdinfo on i915 before Linux 5.19: no drm-* keys at all, so no drm-client-id (#1361).
constexpr std::string_view PRE_5_19_FDINFO = "pos:\t0\nflags:\t02100002\nmnt_id:\t25\nino:\t1234\n";

// #1361: on i915 before 5.19 a client's fdinfo has no drm-client-id, which isn't a closed fd while the
// fd still links to the card: the card has a client without engine stats, so N/A, not idle (0%).
TEST_F(DRMGPUProbeEngineTest, Pre519DrmFdWithoutClientIdLeavesBusynessUnread)
{
    std::filesystem::remove(m_PciDir / "driver");
    std::filesystem::create_symlink("/nonexistent/drivers/i915", m_PciDir / "driver");
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", std::string(PRE_5_19_FDINFO));
    const auto probe = makeProbe();
    for (int sample = 0; sample < 2; ++sample)
    {
        const auto counters = probe->readGPUCounters();
        ASSERT_EQ(counters.size(), 1U);
        EXPECT_FALSE(counters[0].engineBusyAvailable) << "sample " << sample;
        EXPECT_TRUE(counters[0].engineClients.empty());
    }
    // One fdinfo read per sample for the client, kept from one sample to the next.
    const auto before = DRMGPUProbeTestAccessor::fdinfoReads(*probe);
    std::ignore = probe->readGPUCounters();
    EXPECT_EQ(DRMGPUProbeTestAccessor::fdinfoReads(*probe), before + 1U);
    EXPECT_FALSE(utilizationAfterTwoSamples(makeProbe()).has_value());

    // Once the fd closes, the card has no clients: idle.
    std::filesystem::remove_all(m_ProcRoot / "100");
    EXPECT_TRUE(probe->readGPUCounters()[0].engineBusyAvailable);
}

// #1361: the same fdinfo from an fd whose number now names something other than the card's DRM node
// is a closed client's reused fd, dropped as before: with no other clients, the card is idle.
TEST_F(DRMGPUProbeEngineTest, ReusedFdWithoutClientIdIsDropped)
{
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", std::string(PRE_5_19_FDINFO));
    const auto probe = makeProbe();
    const auto first = probe->readGPUCounters();
    ASSERT_EQ(first.size(), 1U);
    EXPECT_FALSE(first[0].engineBusyAvailable); // Still the card's: a client without stats

    const auto link = m_ProcRoot / "100" / "fd" / "4";
    std::filesystem::remove(link);
    std::filesystem::create_symlink("/home/user/notes.txt", link); // Closed, and its number reused
    const auto counters = probe->readGPUCounters();
    EXPECT_TRUE(counters[0].engineClients.empty());
    EXPECT_TRUE(counters[0].engineBusyAvailable); // Idle, not unread
    EXPECT_TRUE(probe->readGPUCounters()[0].engineBusyAvailable);
}

// The issue's regression test: a fake fdinfo tree, two samples, the utilization Domain derives.
TEST_F(DRMGPUProbeEngineTest, GPUModelDerivesUtilizationFromTwoSamples)
{
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", xeFdinfo(7, 1'000, 10'000));
    makeFd(m_ProcRoot, 200, 4, "/dev/dri/renderD129", xeFdinfo(8, 0, 10'000));
    Domain::GPUModel model(makeProbe());
    const auto start = std::chrono::steady_clock::now();
    model.refreshAt(start);
    ASSERT_EQ(model.snapshots().size(), 1U);
    EXPECT_FALSE(model.snapshots()[0].utilizationAvailable); // Nothing to take a change against yet

    // Over 1000 GPU-timestamp cycles, client 7 kept the render engine busy 300 and client 8 200: 50%.
    std::ofstream(m_ProcRoot / "100" / "fdinfo" / "4") << xeFdinfo(7, 1'300, 11'000);
    std::ofstream(m_ProcRoot / "200" / "fdinfo" / "4") << xeFdinfo(8, 200, 11'000);
    model.refreshAt(start + std::chrono::seconds(1));
    const auto snapshots = model.snapshots();
    ASSERT_EQ(snapshots.size(), 1U);
    EXPECT_TRUE(snapshots[0].utilizationAvailable);
    EXPECT_DOUBLE_EQ(snapshots[0].utilizationPercent, 50.0);
}

TEST_F(DRMGPUProbeEngineTest, I915BusyNanosecondsAgainstTheClock)
{
    const auto i915Fdinfo = [](std::uint64_t busyNs)
    {
        return std::format("drm-driver:\ti915\ndrm-pdev:\t0000:03:00.0\ndrm-client-id:\t5\ndrm-engine-render:\t{} ns\n", busyNs);
    };
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", i915Fdinfo(0));
    const auto nowNs = std::make_shared<std::uint64_t>(1'000'000'000);
    Domain::GPUModel model(makeProbe([nowNs] { return *nowNs; }));
    const auto start = std::chrono::steady_clock::now();
    model.refreshAt(start);

    *nowNs += 1'000'000'000; // 1 s, of which the render engine was busy 250 ms
    std::ofstream(m_ProcRoot / "100" / "fdinfo" / "4") << i915Fdinfo(250'000'000);
    model.refreshAt(start + std::chrono::seconds(1));
    const auto snapshots = model.snapshots();
    ASSERT_EQ(snapshots.size(), 1U);
    EXPECT_TRUE(snapshots[0].utilizationAvailable);
    EXPECT_DOUBLE_EQ(snapshots[0].utilizationPercent, 25.0);
}

// #1356: one DRM client open through three fds (two dup'd in one process, one inherited by a child)
// costs one fdinfo read per sample, not three. Each file carries its own cycle count -- which a real
// kernel would not, as they're one file -- so the reading shows which of them was read.
TEST_F(DRMGPUProbeEngineTest, OneClientThroughSeveralFdsIsReadOncePerSample)
{
    const std::array<std::filesystem::path, 3> fdinfos{
        m_ProcRoot / "100" / "fdinfo" / "4", m_ProcRoot / "100" / "fdinfo" / "5", m_ProcRoot / "200" / "fdinfo" / "4"};
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", xeFdinfo(7, 10, 100));
    makeFd(m_ProcRoot, 100, 5, "/dev/dri/renderD129", xeFdinfo(7, 20, 100)); // dup'd
    makeFd(m_ProcRoot, 200, 4, "/dev/dri/renderD129", xeFdinfo(7, 30, 100)); // inherited
    const auto probe = makeProbe();
    // The fdinfo file read for the client this sample, from its cycle count.
    const auto sample = [&probe, &fdinfos]() -> std::optional<std::filesystem::path>
    {
        const auto counters = probe->readGPUCounters();
        EXPECT_TRUE(counters[0].engineBusyAvailable);
        if (counters[0].engineClients.size() != 1U)
        {
            return std::nullopt;
        }
        EXPECT_EQ(counters[0].engineClients[0].clientId, 7U);
        const std::uint64_t cycles = counters[0].engineClients[0].engines.at(RENDER).busy;
        EXPECT_TRUE(cycles == 10U || cycles == 20U || cycles == 30U) << cycles;
        return fdinfos.at(std::min<std::size_t>((cycles / 10U) - 1U, fdinfos.size() - 1U));
    };
    const auto reads = [&probe]
    {
        return DRMGPUProbeTestAccessor::fdinfoReads(*probe);
    };

    // The first sample reads each fd once to learn they are one client.
    const auto first = sample();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(reads(), 3U);

    // From then on, one read per sample, always of the same file.
    for (int i = 0; i < 3; ++i)
    {
        const auto before = reads();
        EXPECT_EQ(sample(), first);
        EXPECT_EQ(reads(), before + 1U);
    }

    // The file read closes: an alias takes over without losing the client, and then is the one read.
    std::filesystem::remove(first.value_or(std::filesystem::path{}));
    auto before = reads();
    const auto second = sample();
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(second, first);
    EXPECT_EQ(reads(), before + 2U); // The closed file, then the alias
    before = reads();
    EXPECT_EQ(sample(), second);
    EXPECT_EQ(reads(), before + 1U);

    // And again for the last alias.
    std::filesystem::remove(second.value_or(std::filesystem::path{}));
    const auto third = sample();
    ASSERT_TRUE(third.has_value());
    EXPECT_NE(third, first);
    EXPECT_NE(third, second);

    // With every fd closed, the client is gone and the card idle.
    std::filesystem::remove(third.value_or(std::filesystem::path{}));
    const auto counters = probe->readGPUCounters();
    EXPECT_TRUE(counters[0].engineClients.empty());
    EXPECT_TRUE(counters[0].engineBusyAvailable);
}

// #1356: the file read for a client whose fd number is reused for another DRM file of the card moves
// to that client; its alias still names the first client, which stays counted.
TEST_F(DRMGPUProbeEngineTest, AReusedFdLeavesItsAliasWithItsClient)
{
    const std::array<std::filesystem::path, 2> fdinfos{m_ProcRoot / "100" / "fdinfo" / "4", m_ProcRoot / "100" / "fdinfo" / "5"};
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", xeFdinfo(7, 10, 100));
    makeFd(m_ProcRoot, 100, 5, "/dev/dri/renderD129", xeFdinfo(7, 20, 100)); // dup'd
    const auto probe = makeProbe();
    std::ignore = probe->readGPUCounters(); // Learns they are one client
    const auto counters = probe->readGPUCounters();
    ASSERT_EQ(counters[0].engineClients.size(), 1U);
    // Which file is read for the client, from its cycle count; the other is the alias.
    const bool firstIsRead = counters[0].engineClients[0].engines.at(RENDER).busy == 10U;
    const auto& read = fdinfos.at(firstIsRead ? 0 : 1);
    const auto& alias = fdinfos.at(firstIsRead ? 1 : 0);

    std::ofstream(read) << xeFdinfo(8, 50, 100); // Closed, and its number reused for client 8
    const auto clientIds = [&probe]
    {
        std::vector<std::uint64_t> ids;
        for (const auto& client : probe->readGPUCounters()[0].engineClients)
        {
            ids.push_back(client.clientId);
        }
        std::ranges::sort(ids);
        return ids;
    };
    auto before = DRMGPUProbeTestAccessor::fdinfoReads(*probe);
    EXPECT_EQ(clientIds(), (std::vector<std::uint64_t>{7, 8}));
    EXPECT_EQ(DRMGPUProbeTestAccessor::fdinfoReads(*probe), before + 2U); // The reused file, then the alias
    before = DRMGPUProbeTestAccessor::fdinfoReads(*probe);
    EXPECT_EQ(clientIds(), (std::vector<std::uint64_t>{7, 8}));
    EXPECT_EQ(DRMGPUProbeTestAccessor::fdinfoReads(*probe), before + 2U); // Now one file per client

    std::filesystem::remove(alias); // Client 7 closes; client 8 stays
    EXPECT_EQ(clientIds(), (std::vector<std::uint64_t>{8}));
}

// #1356: a full rescan keeps the client groups the samples have learned. One client open through three
// fds is still read through the same file after a rescan, and once per sample in steady state; the
// sample after a rescan reads each alias once more, as it may have been reused for another client. A
// path closed before the rescan is dropped from its group, and a newly opened fd of another client is
// read and grouped.
TEST_F(DRMGPUProbeEngineTest, AFullRescanKeepsTheClientsGrouped)
{
    const std::array<std::filesystem::path, 3> fdinfos{
        m_ProcRoot / "100" / "fdinfo" / "4", m_ProcRoot / "100" / "fdinfo" / "5", m_ProcRoot / "200" / "fdinfo" / "4"};
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", xeFdinfo(7, 10, 100));
    makeFd(m_ProcRoot, 100, 5, "/dev/dri/renderD129", xeFdinfo(7, 20, 100)); // dup'd
    makeFd(m_ProcRoot, 200, 4, "/dev/dri/renderD129", xeFdinfo(7, 30, 100)); // inherited
    const auto probe = makeProbe();
    const auto reads = [&probe]
    {
        return DRMGPUProbeTestAccessor::fdinfoReads(*probe);
    };
    // The sample's clients' ids, sorted, and the fdinfo file read for client 7 (from its cycle count).
    std::optional<std::filesystem::path> readFor7;
    const auto sample = [&probe, &fdinfos, &readFor7]
    {
        const auto counters = probe->readGPUCounters();
        EXPECT_TRUE(counters[0].engineBusyAvailable);
        std::vector<std::uint64_t> ids;
        readFor7.reset();
        for (const auto& client : counters[0].engineClients)
        {
            ids.push_back(client.clientId);
            const std::uint64_t cycles = client.engines.at(RENDER).busy;
            if (client.clientId == 7U && cycles >= 10U && cycles <= 30U)
            {
                readFor7 = fdinfos.at((cycles / 10U) - 1U);
            }
        }
        std::ranges::sort(ids);
        return ids;
    };

    EXPECT_EQ(sample(), (std::vector<std::uint64_t>{7}));
    EXPECT_EQ(reads(), 3U); // Each fd once, to learn they are one client
    ASSERT_TRUE(readFor7.has_value());
    const auto first = readFor7.value_or(std::filesystem::path{});

    // A rescan finds the same three fds: the next sample reads the same file for the client, and each
    // alias once to confirm it still names client 7; the samples after it read one file again.
    EXPECT_FALSE(probe->rescanGPUs(GPURescan::Full));
    auto before = reads();
    EXPECT_EQ(sample(), (std::vector<std::uint64_t>{7}));
    EXPECT_EQ(reads(), before + 3U);
    EXPECT_EQ(readFor7, first);
    before = reads();
    EXPECT_EQ(sample(), (std::vector<std::uint64_t>{7}));
    EXPECT_EQ(reads(), before + 1U);
    EXPECT_EQ(readFor7, first);

    // Before the next rescan, the file read for client 7 closes and another client opens the card.
    std::filesystem::remove(first);
    std::filesystem::remove(first.parent_path().parent_path() / "fd" / first.filename());
    makeFd(m_ProcRoot, 300, 6, "/dev/dri/renderD129", xeFdinfo(8, 50, 100));
    EXPECT_FALSE(probe->rescanGPUs(GPURescan::Full));
    // The closed path is gone from client 7's group, so it isn't tried: an alias is read for client 7,
    // the other alias once, and the new path.
    before = reads();
    EXPECT_EQ(sample(), (std::vector<std::uint64_t>{7, 8}));
    EXPECT_EQ(reads(), before + 3U);
    ASSERT_TRUE(readFor7.has_value());
    EXPECT_NE(readFor7, first);
    const auto second = readFor7.value_or(std::filesystem::path{});

    // Grouped now: one read per client in steady state, through rescans too.
    for (int i = 0; i < 2; ++i)
    {
        EXPECT_FALSE(probe->rescanGPUs(GPURescan::Full));
        before = reads();
        EXPECT_EQ(sample(), (std::vector<std::uint64_t>{7, 8}));
        EXPECT_EQ(reads(), before + 3U); // Client 7's file and its alias, and client 8's
        EXPECT_EQ(readFor7, second);
        before = reads();
        EXPECT_EQ(sample(), (std::vector<std::uint64_t>{7, 8}));
        EXPECT_EQ(reads(), before + 2U);
        EXPECT_EQ(readFor7, second);
    }
}

// #1356: an alias is unread while its client's file is, so its fd number could be closed and reused for
// another client's DRM file of the same card -- the same /dev/dri link -- unnoticed by the walk. A full
// rescan ungroups the aliases, so the next sample reads it and finds the new client.
TEST_F(DRMGPUProbeEngineTest, AnAliasReusedForAnotherClientIsFoundAfterAFullRescan)
{
    const std::array<std::filesystem::path, 2> fdinfos{m_ProcRoot / "100" / "fdinfo" / "4", m_ProcRoot / "100" / "fdinfo" / "5"};
    makeFd(m_ProcRoot, 100, 4, "/dev/dri/renderD129", xeFdinfo(7, 10, 100));
    makeFd(m_ProcRoot, 100, 5, "/dev/dri/renderD129", xeFdinfo(7, 20, 100)); // dup'd
    const auto probe = makeProbe();
    const auto clientIds = [&probe]
    {
        std::vector<std::uint64_t> ids;
        for (const auto& client : probe->readGPUCounters()[0].engineClients)
        {
            ids.push_back(client.clientId);
        }
        std::ranges::sort(ids);
        return ids;
    };
    std::ignore = probe->readGPUCounters(); // Learns they are one client
    const auto counters = probe->readGPUCounters();
    ASSERT_EQ(counters[0].engineClients.size(), 1U);
    const bool firstIsRead = counters[0].engineClients[0].engines.at(RENDER).busy == 10U;
    const auto& alias = fdinfos.at(firstIsRead ? 1 : 0);

    // The alias closes and its number is reused for client 9; its link is the same render node.
    std::ofstream(alias) << xeFdinfo(9, 40, 100);
    EXPECT_EQ(clientIds(), (std::vector<std::uint64_t>{7})); // Unread, so not yet seen

    EXPECT_FALSE(probe->rescanGPUs(GPURescan::Full));
    EXPECT_EQ(clientIds(), (std::vector<std::uint64_t>{7, 9}));
    const auto before = DRMGPUProbeTestAccessor::fdinfoReads(*probe);
    EXPECT_EQ(clientIds(), (std::vector<std::uint64_t>{7, 9}));
    EXPECT_EQ(DRMGPUProbeTestAccessor::fdinfoReads(*probe), before + 2U); // One file per client
}

// A client that begins reporting engine stats between the reads of two of its fds: the reading with
// stats counts even when it is read second, under an id already kept from the reading without them.
// Otherwise the card publishes engineBusyAvailable with no counters, which reads as idle.
TEST_F(DRMGPUProbeEngineTest, AnAliasReadWithStatsAfterOneWithoutThemCounts)
{
    // Which of the two fds is read first is up to the directory listing, so cover both ways round:
    // one of them reads the stats-less file first.
    for (const int statsFd : {4, 5})
    {
        SCOPED_TRACE(statsFd);
        const auto procRoot = m_ProcRoot / std::format("stats-on-{}", statsFd);
        const std::string noStats = "drm-driver:\txe\ndrm-pdev:\t0000:03:00.0\ndrm-client-id:\t7\n";
        makeFd(procRoot, 100, 4, "/dev/dri/renderD129", statsFd == 4 ? xeFdinfo(7, 10, 100) : noStats);
        makeFd(procRoot, 100, 5, "/dev/dri/renderD129", statsFd == 5 ? xeFdinfo(7, 10, 100) : noStats);
        const ScriptedVramQuery query; // No VRAM reply: keeps the real ioctl out of these tests
        const auto probe = std::make_unique<DRMGPUProbe>(m_SysRoot.string(), query.fn(), procRoot.string());
        const auto counters = probe->readGPUCounters();
        EXPECT_TRUE(counters[0].engineBusyAvailable);
        ASSERT_EQ(counters[0].engineClients.size(), 1U);
        EXPECT_EQ(counters[0].engineClients[0].clientId, 7U);
    }
}

// =============================================================================
// Discovery and parsing fallbacks (#1548)
// =============================================================================

TEST_F(DRMGPUProbeUnitTest, ACardWhoseNameHasNoIndexIsSkipped)
{
    // "cardX" passes the card-name filter but has no number after "card": skipped, not a crash.
    const auto deviceDir = makeCard("cardX", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");

    DRMGPUProbe probe(m_SysRoot.string());
    EXPECT_TRUE(probe.enumerateGPUs().empty());
}

TEST_F(DRMGPUProbeUnitTest, ACardWithoutADeviceLinkIsSkipped)
{
    // A connector-less card directory with no device/ (or one the probe can't reach) isn't a GPU.
    std::filesystem::create_directories(m_SysRoot / "card0");
    const auto deviceDir = makeCard("card1", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U); // card1 only
}

TEST_F(DRMGPUProbeUnitTest, NvidiaVendorIdReportsNvidia)
{
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x10de");
    writeFile(deviceDir / "class", "0x030000");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].vendor, "NVIDIA");
}

TEST_F(DRMGPUProbeUnitTest, AnOutOfRangeVramTotalCountsAsNoVram)
{
    // A value too large for 64 bits is unreadable, not a huge VRAM: the VGA card stays integrated.
    const auto deviceDir = makeCard("card0", "i915");
    writeFile(deviceDir / "vendor", "0x8086");
    writeFile(deviceDir / "class", "0x030000");
    writeFile(deviceDir / "mem_info_vram_total", "99999999999999999999999");

    DRMGPUProbe probe(m_SysRoot.string());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_TRUE(gpus[0].isIntegrated);
}

TEST(DRMGPUProbeVramQueryTest, RefusesNonIntelDriversAndUnopenableNodes)
{
    // Only i915 and xe answer the memory-region query; nothing is opened for anything else, and a
    // render node that can't be opened is "not known", not an error.
    EXPECT_FALSE(DRMGPUProbe::queryVramByIoctl("", "xe").has_value());
    EXPECT_FALSE(DRMGPUProbe::queryVramByIoctl("/dev/dri/renderD128", "amdgpu").has_value());
    EXPECT_FALSE(DRMGPUProbe::queryVramByIoctl("/nonexistent/dri/renderD999", "xe").has_value());
    EXPECT_FALSE(DRMGPUProbe::queryVramByIoctl("/nonexistent/dri/renderD999", "i915").has_value());
}

} // namespace
} // namespace Platform

#endif // defined(__linux__) && __has_include(<unistd.h>)
