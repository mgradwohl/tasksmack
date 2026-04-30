/// @file test_LinuxPowerProbe.cpp
/// @brief Tests for Platform::LinuxPowerProbe
///
/// Contains two test suites:
///   1. Integration tests that interact with the real /sys/class/power_supply filesystem.
///   2. Unit tests that use a synthetic sysfs directory tree in /tmp, covering battery
///      states and probe logic paths that cannot be reached on real hardware (e.g. CI).

#include <gtest/gtest.h>

#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/Linux/LinuxPowerProbe.h"
#include "Platform/PowerTypes.h"

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
// Integration Tests (real /sys/class/power_supply)
// =============================================================================

TEST(LinuxPowerProbeTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ LinuxPowerProbe probe; });
}

TEST(LinuxPowerProbeTest, CapabilitiesReportedCorrectly)
{
    LinuxPowerProbe probe;
    auto caps = probe.capabilities();

    // If there's no battery, most other capabilities should be false
    if (!caps.hasBattery)
    {
        EXPECT_FALSE(caps.hasChargePercent);
        EXPECT_FALSE(caps.hasChargeCapacity);
    }
}

TEST(LinuxPowerProbeTest, ReadSucceeds)
{
    LinuxPowerProbe probe;
    PowerCounters counters;

    EXPECT_NO_THROW({ counters = probe.read(); });

    // Verify basic structure validity
    auto caps = probe.capabilities();
    if (caps.hasBattery)
    {
        // If battery is present, charge percent should be valid range or -1
        EXPECT_TRUE(counters.chargePercent == -1 || (counters.chargePercent >= 0 && counters.chargePercent <= 100));

        // Battery state should be one of the valid states
        EXPECT_TRUE(counters.state == BatteryState::Unknown || counters.state == BatteryState::Charging ||
                    counters.state == BatteryState::Discharging || counters.state == BatteryState::Full ||
                    counters.state == BatteryState::NotPresent);
    }
    else
    {
        // No battery present
        EXPECT_EQ(counters.state, BatteryState::NotPresent);
        EXPECT_TRUE(counters.isOnAc);
    }
}

TEST(LinuxPowerProbeTest, MultipleReadsAreConsistent)
{
    LinuxPowerProbe probe;
    auto caps = probe.capabilities();

    if (!caps.hasBattery)
    {
        GTEST_SKIP() << "No battery detected, skipping consistency test";
    }

    auto counters1 = probe.read();
    auto counters2 = probe.read();

    // State should be consistent between quick successive reads
    EXPECT_EQ(counters1.state, counters2.state);
    EXPECT_EQ(counters1.isOnAc, counters2.isOnAc);

    // Charge percent shouldn't change dramatically in quick succession
    if (counters1.chargePercent >= 0 && counters2.chargePercent >= 0)
    {
        int diff = std::abs(counters1.chargePercent - counters2.chargePercent);
        EXPECT_LE(diff, 1) << "Charge percent changed too much between reads";
    }
}

// =============================================================================
// Battery State Validation
// =============================================================================

TEST(LinuxPowerProbeTest, BatteryStateIsValid)
{
    LinuxPowerProbe probe;
    auto counters = probe.read();
    auto caps = probe.capabilities();

    if (!caps.hasBattery)
    {
        GTEST_SKIP() << "No battery detected";
    }

    // Verify state is consistent with AC status
    if (counters.isOnAc)
    {
        EXPECT_TRUE(counters.state == BatteryState::Charging || counters.state == BatteryState::Full ||
                    counters.state == BatteryState::Unknown);
    }
    else
    {
        EXPECT_TRUE(counters.state == BatteryState::Discharging || counters.state == BatteryState::Unknown);
    }
}

TEST(LinuxPowerProbeTest, ChargePercentInValidRange)
{
    LinuxPowerProbe probe;
    auto counters = probe.read();
    auto caps = probe.capabilities();

    if (!caps.hasChargePercent)
    {
        GTEST_SKIP() << "Charge percent not available";
    }

    // Charge percent should be 0-100 or -1 (unavailable)
    EXPECT_TRUE(counters.chargePercent == -1 || (counters.chargePercent >= 0 && counters.chargePercent <= 100));
}

// =============================================================================
// Unit Tests — synthetic sysfs fixture
// =============================================================================

/// Fixture that creates a temporary sysfs-like directory tree for each test.
/// Battery devices are created by the individual tests using the helper methods.
class LinuxPowerProbeUnitTest : public ::testing::Test
{
  protected:
    std::filesystem::path m_SysRoot; // e.g. /tmp/tasksmack_power_test_<N>

    void SetUp() override
    {
        // Include PID and a per-process atomic counter so parallel test
        // processes (gtest_discover_tests can run cases concurrently) never
        // collide on the same temporary directory.
        static std::atomic<int> s_counter{0};
        const auto seq = s_counter.fetch_add(1, std::memory_order_relaxed);
        const auto name = "tasksmack_power_test_" + std::to_string(getpid()) + "_" + std::to_string(seq);
        m_SysRoot = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(m_SysRoot);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_SysRoot, ec);
    }

    // Create a battery device subdirectory with a "type" file set to "Battery"
    // Returns the path to the created device directory.
    [[nodiscard]] std::filesystem::path makeBatteryDevice(const std::string& name) const
    {
        const auto devPath = m_SysRoot / name;
        std::filesystem::create_directories(devPath);
        writeFile(devPath / "type", "Battery");
        return devPath;
    }

    // Create a non-battery power supply device (e.g. AC adapter)
    [[nodiscard]] std::filesystem::path makeAcDevice(const std::string& name) const
    {
        const auto devPath = m_SysRoot / name;
        std::filesystem::create_directories(devPath);
        writeFile(devPath / "type", "Mains");
        return devPath;
    }

    static void writeFile(const std::filesystem::path& path, const std::string& content)
    {
        std::ofstream f(path);
        f << content << "\n";
    }
};

// -----------------------------------------------------------------------
// AC-only (no battery) scenarios
// -----------------------------------------------------------------------

TEST_F(LinuxPowerProbeUnitTest, AcOnly_NoDirectory_ReturnsNotPresent)
{
    // Power supply root directory does not exist at all
    const auto nonExistent = m_SysRoot / "nonexistent";
    LinuxPowerProbe probe(nonExistent.string());

    auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasBattery);

    auto counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::NotPresent);
    EXPECT_TRUE(counters.isOnAc);
}

TEST_F(LinuxPowerProbeUnitTest, AcOnly_EmptyDirectory_ReturnsNotPresent)
{
    // Directory exists but contains no entries
    LinuxPowerProbe probe(m_SysRoot.string());

    auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasBattery);

    auto counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::NotPresent);
    EXPECT_TRUE(counters.isOnAc);
}

TEST_F(LinuxPowerProbeUnitTest, AcOnly_OnlyAcAdapter_ReturnsNotPresent)
{
    // Directory only has an AC adapter (type = "Mains")
    [[maybe_unused]] const auto acPath = makeAcDevice("AC0");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasBattery);

    auto counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::NotPresent);
    EXPECT_TRUE(counters.isOnAc);
}

TEST_F(LinuxPowerProbeUnitTest, AcOnly_DeviceWithNoTypeFile_IsIgnored)
{
    // Device directory exists but has no "type" file
    std::filesystem::create_directories(m_SysRoot / "BAT0");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasBattery);
}

// -----------------------------------------------------------------------
// Battery state parsing
// -----------------------------------------------------------------------

TEST_F(LinuxPowerProbeUnitTest, Battery_Discharging_ReturnsCorrectState)
{
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Discharging");
    writeFile(batPath / "capacity", "75");

    LinuxPowerProbe probe(m_SysRoot.string());

    EXPECT_TRUE(probe.capabilities().hasBattery);
    EXPECT_TRUE(probe.capabilities().hasChargePercent);

    auto counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::Discharging);
    EXPECT_FALSE(counters.isOnAc);
    EXPECT_EQ(counters.chargePercent, 75);
}

TEST_F(LinuxPowerProbeUnitTest, Battery_Charging_ReturnsCorrectState)
{
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Charging");
    writeFile(batPath / "capacity", "50");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::Charging);
    EXPECT_TRUE(counters.isOnAc);
    EXPECT_EQ(counters.chargePercent, 50);
}

TEST_F(LinuxPowerProbeUnitTest, Battery_Full_ReturnsCorrectState)
{
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Full");
    writeFile(batPath / "capacity", "100");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::Full);
    EXPECT_TRUE(counters.isOnAc);
    EXPECT_EQ(counters.chargePercent, 100);
}

TEST_F(LinuxPowerProbeUnitTest, Battery_NotCharging_TreatedAsFull)
{
    // "Not charging" (plugged in but at 100%) maps to Full state
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Not charging");
    writeFile(batPath / "capacity", "100");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::Full);
    EXPECT_TRUE(counters.isOnAc);
}

TEST_F(LinuxPowerProbeUnitTest, Battery_UnknownStatus_ReturnsUnknown)
{
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Some_Unrecognised_State");
    writeFile(batPath / "capacity", "42");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::Unknown);
}

TEST_F(LinuxPowerProbeUnitTest, Battery_MissingStatusFile_StateIsUnknown)
{
    // Battery device exists but has no "status" file → empty string → Unknown
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "capacity", "80");

    LinuxPowerProbe probe(m_SysRoot.string());

    EXPECT_TRUE(probe.capabilities().hasBattery);
    auto counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::Unknown);
}

// -----------------------------------------------------------------------
// Energy/charge counter flavours
// -----------------------------------------------------------------------

TEST_F(LinuxPowerProbeUnitTest, Battery_EnergyCounters_ParsedCorrectly)
{
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Discharging");
    writeFile(batPath / "capacity", "60");
    // energy values in µWh
    writeFile(batPath / "energy_now", "36000000");         // 36 Wh
    writeFile(batPath / "energy_full", "60000000");        // 60 Wh
    writeFile(batPath / "energy_full_design", "65000000"); // 65 Wh design

    LinuxPowerProbe probe(m_SysRoot.string());

    auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasChargeCapacity);
    EXPECT_TRUE(caps.hasHealthPercent);

    auto counters = probe.read();
    EXPECT_DOUBLE_EQ(counters.chargeNowWh, 36.0);
    EXPECT_DOUBLE_EQ(counters.chargeFullWh, 60.0);
    EXPECT_DOUBLE_EQ(counters.chargeDesignWh, 65.0);

    // healthPercent = (60 / 65) * 100 ≈ 92
    EXPECT_EQ(counters.healthPercent, 92);
}

TEST_F(LinuxPowerProbeUnitTest, Battery_ChargeCounters_ConvertedViaVoltage)
{
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Discharging");
    writeFile(batPath / "capacity", "50");
    // charge-based (µAh) + voltage (µV), no energy_now
    writeFile(batPath / "charge_now", "2000000");         // 2 Ah = 2000000 µAh
    writeFile(batPath / "charge_full", "4000000");        // 4 Ah
    writeFile(batPath / "charge_full_design", "4500000"); // 4.5 Ah
    writeFile(batPath / "voltage_now", "11000000");       // 11 V = 11000000 µV

    LinuxPowerProbe probe(m_SysRoot.string());

    auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasChargeCapacity);

    auto counters = probe.read();
    // chargeNowWh = (2000000 µAh * 11000000 µV) / 1e12 = 22 Wh
    EXPECT_NEAR(counters.chargeNowWh, 22.0, 0.01);
    // chargeFullWh = (4000000 * 11000000) / 1e12 = 44 Wh
    EXPECT_NEAR(counters.chargeFullWh, 44.0, 0.01);
}

TEST_F(LinuxPowerProbeUnitTest, Battery_ChargeCounters_ZeroVoltage_NoConversion)
{
    // voltage_now is 0 → conversion must not occur (avoid division / invalid results)
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Discharging");
    writeFile(batPath / "capacity", "50");
    writeFile(batPath / "charge_now", "2000000");
    writeFile(batPath / "charge_full", "4000000");
    writeFile(batPath / "charge_full_design", "4500000");
    writeFile(batPath / "voltage_now", "0");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto counters = probe.read();
    // With zero voltage the Wh conversion is skipped, so values remain 0
    EXPECT_DOUBLE_EQ(counters.chargeNowWh, 0.0);
    EXPECT_DOUBLE_EQ(counters.chargeFullWh, 0.0);
}

// -----------------------------------------------------------------------
// Power-rate flavours
// -----------------------------------------------------------------------

TEST_F(LinuxPowerProbeUnitTest, Battery_PowerNow_Discharging_PositiveRate)
{
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Discharging");
    writeFile(batPath / "capacity", "70");
    writeFile(batPath / "power_now", "12000000"); // 12 W in µW
    writeFile(batPath / "energy_now", "50000000");
    writeFile(batPath / "energy_full", "70000000");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasPowerRate);

    auto counters = probe.read();
    EXPECT_DOUBLE_EQ(counters.powerNowW, 12.0); // positive while discharging
    // time to empty ≈ 50 Wh / 12 W * 3600 ≈ 15000 s
    EXPECT_GT(counters.timeToEmptySec, 0ULL);
    EXPECT_EQ(counters.timeToFullSec, 0ULL);
}

TEST_F(LinuxPowerProbeUnitTest, Battery_PowerNow_Charging_NegativeRate)
{
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Charging");
    writeFile(batPath / "capacity", "80");
    writeFile(batPath / "power_now", "15000000");   // 15 W in µW
    writeFile(batPath / "energy_now", "56000000");  // 56 Wh
    writeFile(batPath / "energy_full", "70000000"); // 70 Wh

    LinuxPowerProbe probe(m_SysRoot.string());

    auto counters = probe.read();
    // Charging → power_now negated
    EXPECT_DOUBLE_EQ(counters.powerNowW, -15.0);
    // time to full = (70-56) / 15 * 3600 = 3360 s
    EXPECT_GT(counters.timeToFullSec, 0ULL);
    EXPECT_EQ(counters.timeToEmptySec, 0ULL);
}

TEST_F(LinuxPowerProbeUnitTest, Battery_CurrentNow_UsesVoltageForPower)
{
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Discharging");
    writeFile(batPath / "capacity", "50");
    // Use current_now (no power_now file)
    writeFile(batPath / "current_now", "2000000");  // 2 A = 2000000 µA
    writeFile(batPath / "voltage_now", "12000000"); // 12 V = 12000000 µV

    LinuxPowerProbe probe(m_SysRoot.string());

    auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasPowerRate);

    auto counters = probe.read();
    // powerNowW = (2000000 µA * 12000000 µV) / 1e12 = 24 W
    EXPECT_NEAR(counters.powerNowW, 24.0, 0.01);
}

TEST_F(LinuxPowerProbeUnitTest, Battery_CurrentNow_ZeroVoltage_NoPowerComputed)
{
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Discharging");
    writeFile(batPath / "capacity", "50");
    writeFile(batPath / "current_now", "2000000");
    writeFile(batPath / "voltage_now", "0");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto counters = probe.read();
    // Zero voltage → power stays at 0
    EXPECT_DOUBLE_EQ(counters.powerNowW, 0.0);
}

// -----------------------------------------------------------------------
// Optional field pass-through
// -----------------------------------------------------------------------

TEST_F(LinuxPowerProbeUnitTest, Battery_TechnologyAndModel_ParsedCorrectly)
{
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Discharging");
    writeFile(batPath / "capacity", "90");
    writeFile(batPath / "technology", "Li-ion");
    writeFile(batPath / "model_name", "DELL 1234");
    writeFile(batPath / "manufacturer", "SimpelBatt");
    writeFile(batPath / "cycle_count", "123");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasTechnology);
    EXPECT_TRUE(caps.hasCycleCount);

    auto counters = probe.read();
    EXPECT_EQ(counters.technology, "Li-ion");
    EXPECT_EQ(counters.model, "DELL 1234");
    EXPECT_EQ(counters.manufacturer, "SimpelBatt");
    EXPECT_EQ(counters.cycleCount, 123ULL);
}

TEST_F(LinuxPowerProbeUnitTest, Battery_VoltageNow_ConvertedToMillivolts)
{
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Discharging");
    writeFile(batPath / "capacity", "60");
    writeFile(batPath / "voltage_now", "11500000"); // 11.5 V = 11500000 µV → 11500 mV

    LinuxPowerProbe probe(m_SysRoot.string());

    auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasVoltage);

    auto counters = probe.read();
    EXPECT_EQ(counters.voltageNowMv, 11500ULL);
}

// -----------------------------------------------------------------------
// Multiple batteries
// -----------------------------------------------------------------------

TEST_F(LinuxPowerProbeUnitTest, MultipleBatteries_PrimaryUsedForRead)
{
    // Two primary batteries exist: BAT0 and BAT1.
    // The probe reads only the first discovered primary battery; regardless of which
    // one the filesystem iterator returns first, a valid battery state must be reported.
    const auto bat0 = makeBatteryDevice("BAT0");
    writeFile(bat0 / "status", "Discharging");
    writeFile(bat0 / "capacity", "40");

    const auto bat1 = makeBatteryDevice("BAT1");
    writeFile(bat1 / "status", "Discharging");
    writeFile(bat1 / "capacity", "40");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasBattery);

    auto counters = probe.read();
    // Both batteries report the same state to make the test order-independent.
    EXPECT_EQ(counters.state, BatteryState::Discharging);
    EXPECT_EQ(counters.chargePercent, 40);
}

TEST_F(LinuxPowerProbeUnitTest, PeripheralBattery_DiscoveredWhenNoPrimary)
{
    // "hidpp_battery_0" does not start with "BAT" or "CMB" but is still a Battery type
    const auto periph = makeBatteryDevice("hidpp_battery_0");
    writeFile(periph / "status", "Discharging");
    writeFile(periph / "capacity", "88");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasBattery);

    auto counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::Discharging);
    EXPECT_EQ(counters.chargePercent, 88);
}

TEST_F(LinuxPowerProbeUnitTest, PrimaryBatteryPreferredOverPeripheral)
{
    // BAT0 is primary; hidpp_battery_0 is peripheral.
    // BAT0 should be used for the read (primary placed first in m_BatteryPaths).
    const auto periph = makeBatteryDevice("hidpp_battery_0");
    writeFile(periph / "status", "Discharging");
    writeFile(periph / "capacity", "55");

    const auto bat = makeBatteryDevice("BAT0");
    writeFile(bat / "status", "Full");
    writeFile(bat / "capacity", "100");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::Full);
    EXPECT_EQ(counters.chargePercent, 100);
}

TEST_F(LinuxPowerProbeUnitTest, CmbBatteryTreatedAsPrimary)
{
    // ThinkPad batteries are named "CMB0"
    const auto cmb = makeBatteryDevice("CMB0");
    writeFile(cmb / "status", "Discharging");
    writeFile(cmb / "capacity", "65");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasBattery);

    auto counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::Discharging);
    EXPECT_EQ(counters.chargePercent, 65);
}

// -----------------------------------------------------------------------
// Malformed / missing sysfs files
// -----------------------------------------------------------------------

TEST_F(LinuxPowerProbeUnitTest, Battery_MalformedCapacity_FallsBackToMinusOne)
{
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Discharging");
    writeFile(batPath / "capacity", "not_a_number");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto counters = probe.read();
    // readSysfsInt falls back to -1 on parse error
    EXPECT_EQ(counters.chargePercent, -1);
}

TEST_F(LinuxPowerProbeUnitTest, Battery_MalformedEnergyNow_FallsBackToZero)
{
    const auto batPath = makeBatteryDevice("BAT0");
    writeFile(batPath / "status", "Discharging");
    writeFile(batPath / "capacity", "50");
    writeFile(batPath / "energy_now", "not_a_number");
    writeFile(batPath / "energy_full", "60000000");
    writeFile(batPath / "energy_full_design", "65000000");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto counters = probe.read();
    // readSysfsUInt64 falls back to 0 on parse error
    EXPECT_DOUBLE_EQ(counters.chargeNowWh, 0.0);
}

TEST_F(LinuxPowerProbeUnitTest, Battery_WhitespaceStrippedFromSysfsFiles)
{
    const auto batPath = makeBatteryDevice("BAT0");
    // sysfs files often have trailing newline; the probe should strip it
    writeFile(batPath / "status", "  Discharging  ");
    writeFile(batPath / "capacity", "  77  ");

    LinuxPowerProbe probe(m_SysRoot.string());

    auto counters = probe.read();
    EXPECT_EQ(counters.state, BatteryState::Discharging);
    EXPECT_EQ(counters.chargePercent, 77);
}

} // namespace
} // namespace Platform

#endif // defined(__linux__) && __has_include(<unistd.h>)
