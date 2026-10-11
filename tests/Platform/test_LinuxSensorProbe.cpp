/// @file test_LinuxSensorProbe.cpp
/// @brief The live Sensors section's Linux read (#1522): discover() lists what LinuxSensors.h lists for
/// the System Information page, and readValues() re-reads the files it opened, in the same order, so a
/// changed value shows without finding the sensors again; a chip whose device is runtime-suspended
/// isn't read; and the real probe on this machine, timed per tick.

#include "Platform/ISensorProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxSensorProbe.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iostream>
#include <ratio>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform
{
namespace
{

/// A fresh, empty fixture root under the temp directory, removed (best effort) when the test ends.
class FixtureRoot
{
  public:
    FixtureRoot()
        : m_Path(std::filesystem::temp_directory_path() /
                 std::format("ts_sensor_probe_{}_{}", s_Counter.fetch_add(1), std::chrono::steady_clock::now().time_since_epoch().count()))
    {
        std::filesystem::create_directories(m_Path);
    }
    ~FixtureRoot() noexcept
    {
        try
        {
            std::error_code ec;
            std::filesystem::remove_all(m_Path, ec); // best effort
        }
        catch (...) // NOLINT(bugprone-empty-catch) - a destructor must not throw; the temp dir is left behind
        {}
    }
    FixtureRoot(const FixtureRoot&) = delete;
    FixtureRoot& operator=(const FixtureRoot&) = delete;
    FixtureRoot(FixtureRoot&&) = delete;
    FixtureRoot& operator=(FixtureRoot&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_Path;
    }

    /// Writes in place (truncating the same file), as sysfs presents a new value to an open descriptor.
    void write(std::string_view relative, std::string_view text) const
    {
        const std::filesystem::path file = m_Path / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
    }

  private:
    std::filesystem::path m_Path;
    static inline std::atomic<int> s_Counter{0};
};

TEST(LinuxSensorProbeTest, ReadsTheDiscoveredFilesAgainInOrder)
{
    const FixtureRoot root;
    root.write("sys/class/hwmon/hwmon0/name", "nct6798\n");
    root.write("sys/class/hwmon/hwmon0/fan1_input", "1180\n");
    root.write("sys/class/hwmon/hwmon0/in0_input", "12180\n");
    root.write("sys/class/hwmon/hwmon0/power1_input", "15200000\n");
    root.write("sys/class/hwmon/hwmon1/name", "coretemp\n");
    root.write("sys/class/hwmon/hwmon1/temp1_input", "52000\n");
    root.write("sys/class/thermal/thermal_zone0/type", "x86_pkg_temp\n");
    root.write("sys/class/thermal/thermal_zone0/temp", "51000\n");

    LinuxSensorProbe probe(root.path());
    const SensorsInfo info = probe.discover();
    ASSERT_EQ(sensorCount(info), 5U);
    ASSERT_EQ(info.devices.size(), 3U);
    EXPECT_EQ(info.devices[0].readings[0].label, "fan1");
    EXPECT_EQ(info.devices[2].name, "Thermal zones");

    // New values, read through the descriptors discover() opened: nothing is listed again.
    root.write("sys/class/hwmon/hwmon0/fan1_input", "1250\n");
    root.write("sys/class/hwmon/hwmon0/in0_input", "12000\n");
    root.write("sys/class/hwmon/hwmon0/power1_input", "20500000\n");
    root.write("sys/class/hwmon/hwmon1/temp1_input", "61500\n");
    root.write("sys/class/thermal/thermal_zone0/temp", "60000\n");
    root.write("sys/class/hwmon/hwmon2/name", "nvme\n"); // appears after discovery: not read
    root.write("sys/class/hwmon/hwmon2/temp1_input", "40000\n");

    std::vector<double> values(sensorCount(info) + 1, -1.0);
    probe.readValues(values);
    EXPECT_DOUBLE_EQ(values[0], 1250.0);
    EXPECT_DOUBLE_EQ(values[1], 12.0);
    EXPECT_DOUBLE_EQ(values[2], 20.5);
    EXPECT_DOUBLE_EQ(values[3], 61.5);
    EXPECT_DOUBLE_EQ(values[4], 60.0);
    EXPECT_DOUBLE_EQ(values[5], -1.0); // beyond the sensors found: left alone
}

TEST(LinuxSensorProbeTest, AnUnreadableValueIsNaN)
{
    const FixtureRoot root;
    root.write("sys/class/hwmon/hwmon0/name", "nct6798\n");
    root.write("sys/class/hwmon/hwmon0/fan1_input", "1180\n");
    LinuxSensorProbe probe(root.path());
    ASSERT_EQ(sensorCount(probe.discover()), 1U);

    root.write("sys/class/hwmon/hwmon0/fan1_input", ""); // a disconnected header reads as an error
    std::vector<double> values(1, 0.0);
    probe.readValues(values);
    EXPECT_TRUE(std::isnan(values[0]));
}

TEST(LinuxSensorProbeTest, ASuspendedDeviceIsNotWoken)
{
    const FixtureRoot root;
    root.write("sys/class/hwmon/hwmon0/name", "amdgpu\n");
    root.write("sys/class/hwmon/hwmon0/temp1_input", "45000\n");
    root.write("sys/class/hwmon/hwmon0/device/power/runtime_status", "active\n");
    root.write("sys/class/hwmon/hwmon1/name", "coretemp\n");
    root.write("sys/class/hwmon/hwmon1/temp1_input", "50000\n");
    LinuxSensorProbe probe(root.path());
    ASSERT_EQ(sensorCount(probe.discover()), 2U);

    std::vector<double> values(2, 0.0);
    probe.readValues(values);
    EXPECT_DOUBLE_EQ(values[0], 45.0);

    root.write("sys/class/hwmon/hwmon0/device/power/runtime_status", "suspended\n");
    probe.readValues(values);
    EXPECT_TRUE(std::isnan(values[0])); // a gap rather than a wake-up
    EXPECT_DOUBLE_EQ(values[1], 50.0);  // other chips are read as usual

    root.write("sys/class/hwmon/hwmon0/device/power/runtime_status", "active\n");
    probe.readValues(values);
    EXPECT_DOUBLE_EQ(values[0], 45.0);
}

TEST(LinuxSensorProbeTest, DiscoveringAgainStartsOver)
{
    const FixtureRoot root;
    root.write("sys/class/hwmon/hwmon0/name", "coretemp\n");
    root.write("sys/class/hwmon/hwmon0/temp1_input", "50000\n");
    LinuxSensorProbe probe(root.path());
    ASSERT_EQ(sensorCount(probe.discover()), 1U);
    root.write("sys/class/hwmon/hwmon1/name", "nvme\n");
    root.write("sys/class/hwmon/hwmon1/temp1_input", "40000\n");
    ASSERT_EQ(sensorCount(probe.discover()), 2U);
    std::vector<double> values(2, 0.0);
    probe.readValues(values);
    EXPECT_DOUBLE_EQ(values[1], 40.0);
}

/// The real probe on this machine, timed per tick: the figure the PR reports (#1522).
TEST(LinuxSensorProbeTest, ReadsThisMachineCheaply)
{
    LinuxSensorProbe probe;
    const SensorsInfo info = probe.discover();
    EXPECT_TRUE(info.available);
    std::vector<double> values(sensorCount(info), 0.0);
    constexpr int TICKS = 1000;
    const auto start = std::chrono::steady_clock::now();
    int tick = 0;
    while (tick < TICKS)
    {
        probe.readValues(values);
        ++tick;
    }
    const auto perTick = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / TICKS;
    std::cout << std::format("[ sensors  ] {} sensors, {:.2f} us per readValues()\n", values.size(), perTick);
    RecordProperty("sensors", static_cast<int>(values.size()));
    RecordProperty("microsecondsPerTick", std::format("{:.2f}", perTick));
}

} // namespace
} // namespace Platform
