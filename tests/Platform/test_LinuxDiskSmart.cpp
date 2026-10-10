/// @file test_LinuxDiskSmart.cpp
/// @brief Linux disk SMART health through udisks2 (#1631): udisks2's object paths and units, and how
/// a drive's read becomes PhysicalDisk's health or the reason it has none. No bus needed.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxDiskSmart.h"
#include "Platform/Linux/LinuxStorage.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

using LinuxDiskSmart::DriveSmart;
using LinuxDiskSmart::DriveSmartRead;

DriveSmart ataSmart()
{
    DriveSmart smart;
    smart.kind = DriveSmart::Kind::Ata;
    smart.updated = 1700000000;
    smart.smartSupported = true;
    smart.smartEnabled = true;
    smart.temperatureKelvin = 308.15;
    smart.powerOnSeconds = 12345ULL * 3600;
    smart.badSectors = 0;
    return smart;
}

DriveSmart nvmeSmart()
{
    DriveSmart smart;
    smart.kind = DriveSmart::Kind::Nvme;
    smart.updated = 1700000000;
    smart.nvmeTemperatureKelvin = 314;
    smart.attributesRead = true;
    smart.percentUsed = 3;
    smart.availableSpare = 100;
    smart.mediaErrors = 0;
    return smart;
}

DriveSmartRead answered(const DriveSmart& smart)
{
    return {.smart = smart, .error = {}};
}

TEST(LinuxDiskSmartTest, BlockObjectPathsEscapeAsUdisks2Does)
{
    EXPECT_EQ(LinuxDiskSmart::blockObjectPath("nvme0n1"), "/org/freedesktop/UDisks2/block_devices/nvme0n1");
    EXPECT_EQ(LinuxDiskSmart::blockObjectPath("sda"), "/org/freedesktop/UDisks2/block_devices/sda");
    EXPECT_EQ(LinuxDiskSmart::blockObjectPath("dm-0"), "/org/freedesktop/UDisks2/block_devices/dm_2d0");
    EXPECT_EQ(LinuxDiskSmart::blockObjectPath("a.b"), "/org/freedesktop/UDisks2/block_devices/a_2eb");
}

TEST(LinuxDiskSmartTest, CriticalWarningsBecomeTheHealthLogBits)
{
    EXPECT_EQ(LinuxDiskSmart::criticalWarningBits({}), 0);
    EXPECT_EQ(LinuxDiskSmart::criticalWarningBits({"spare"}), 0x01);
    EXPECT_EQ(LinuxDiskSmart::criticalWarningBits({"degraded", "readonly"}), 0x0C);
    EXPECT_EQ(LinuxDiskSmart::criticalWarningBits({"temperature", "volatile_mem", "pmr_readonly"}), 0x32);
    EXPECT_EQ(LinuxDiskSmart::criticalWarningBits({"something new"}), 0); // a name this build doesn't know
}

TEST(LinuxDiskSmartTest, KelvinBecomesWholeDegreesCelsius)
{
    EXPECT_EQ(LinuxDiskSmart::celsiusFromKelvin(308.15), 35);
    EXPECT_EQ(LinuxDiskSmart::celsiusFromKelvin(314.0), 41);
    EXPECT_FALSE(LinuxDiskSmart::celsiusFromKelvin(0.0).has_value()); // udisks2's "unknown"
    EXPECT_FALSE(LinuxDiskSmart::celsiusFromKelvin(1000.0).has_value());
    EXPECT_FALSE(LinuxDiskSmart::celsiusFromKelvin(std::numeric_limits<double>::quiet_NaN()).has_value());
}

TEST(LinuxDiskSmartTest, AnAtaDrivesSmartStatusIsItsHealth)
{
    PhysicalDisk disk;
    disk.healthUnavailableReason = "SMART status wasn't read";
    LinuxDiskSmart::applySmart(disk, answered(ataSmart()));
    ASSERT_TRUE(disk.ataHealth.has_value());
    const AtaHealth health = disk.ataHealth.value_or(AtaHealth{.failing = true, .badSectors = {}, .powerOnHours = {}});
    EXPECT_FALSE(health.failing);
    EXPECT_EQ(health.badSectors, std::optional<std::uint64_t>(0));
    EXPECT_EQ(health.powerOnHours, std::optional<std::uint64_t>(12345));
    EXPECT_FALSE(disk.health.has_value());
    EXPECT_TRUE(disk.healthUnavailableReason.empty());
    EXPECT_EQ(disk.temperatureCelsius, 35); // no hwmon reading, so udisks2's

    DriveSmart failing = ataSmart();
    failing.failing = true;
    failing.badSectors = -1;    // unknown
    failing.powerOnSeconds = 0; // unknown
    PhysicalDisk dying;
    dying.temperatureCelsius = 50; // hwmon's reading wins
    LinuxDiskSmart::applySmart(dying, answered(failing));
    ASSERT_TRUE(dying.ataHealth.has_value());
    const AtaHealth dyingHealth = dying.ataHealth.value_or(AtaHealth{});
    EXPECT_TRUE(dyingHealth.failing);
    EXPECT_FALSE(dyingHealth.badSectors.has_value());
    EXPECT_FALSE(dyingHealth.powerOnHours.has_value());
    EXPECT_EQ(dying.temperatureCelsius, 50);
}

TEST(LinuxDiskSmartTest, AnNvmeDrivesHealthLogIsItsHealth)
{
    DriveSmart smart = nvmeSmart();
    smart.criticalWarnings = {"spare"};
    PhysicalDisk disk;
    LinuxDiskSmart::applySmart(disk, answered(smart));
    ASSERT_TRUE(disk.health.has_value());
    const NvmeHealth health = disk.health.value_or(NvmeHealth{});
    EXPECT_EQ(health.criticalWarning, 0x01);
    EXPECT_EQ(health.percentageUsed, 3);
    EXPECT_EQ(health.availableSparePercent, 100);
    EXPECT_EQ(health.mediaErrors, 0U);
    EXPECT_FALSE(disk.ataHealth.has_value());
    EXPECT_TRUE(disk.healthUnavailableReason.empty());
    EXPECT_EQ(disk.temperatureCelsius, 41);
}

TEST(LinuxDiskSmartTest, AnNvmeDriveWhoseHealthLogWasRefusedSaysWhy)
{
    DriveSmart smart = nvmeSmart();
    smart.attributesRead = false;
    smart.attributesError = "Not authorized";
    PhysicalDisk disk;
    LinuxDiskSmart::applySmart(disk, answered(smart));
    EXPECT_FALSE(disk.health.has_value());
    EXPECT_EQ(disk.healthUnavailableReason, "udisks2 didn't return the health log: Not authorized");
    EXPECT_EQ(disk.temperatureCelsius, 41); // the controller's temperature still reads
}

TEST(LinuxDiskSmartTest, EveryOtherCaseKeepsTheMutedDashWithItsReason)
{
    const auto reasonFor = [](const DriveSmartRead& read)
    {
        PhysicalDisk disk;
        LinuxDiskSmart::applySmart(disk, read);
        EXPECT_FALSE(disk.health.has_value());
        EXPECT_FALSE(disk.ataHealth.has_value());
        return disk.healthUnavailableReason;
    };
    EXPECT_EQ(reasonFor({.smart = std::nullopt, .error = "udisks2 isn't running, so SMART status can't be read"}),
              "udisks2 isn't running, so SMART status can't be read");
    EXPECT_EQ(reasonFor({.smart = DriveSmart{}, .error = {}}), "udisks2 has no SMART data for this drive");

    DriveSmart unsupported = ataSmart();
    unsupported.smartSupported = false;
    EXPECT_EQ(reasonFor(answered(unsupported)), "This drive doesn't support SMART");
    DriveSmart disabled = ataSmart();
    disabled.smartEnabled = false;
    EXPECT_EQ(reasonFor(answered(disabled)), "SMART is turned off on this drive");
    DriveSmart notYet = nvmeSmart();
    notYet.updated = 0;
    EXPECT_EQ(reasonFor(answered(notYet)), "udisks2 hasn't read this drive's SMART data yet");
}

TEST(LinuxDiskSmartTest, ReadStorageFactsAsksTheReaderForEachDisk)
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("ts_smart_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(root);
    const auto write = [&root](const std::string& relative, const std::string& text)
    {
        std::filesystem::create_directories((root / relative).parent_path());
        std::ofstream(root / relative) << text;
    };
    for (const std::string name : {"nvme0n1", "sda", "sdb"})
    {
        write("sys/block/" + name + "/size", "1000\n");
        write("sys/block/" + name + "/device/model", "Disk\n");
    }

    std::vector<std::string> asked;
    const LinuxDiskSmart::SmartReader reader = [&asked](const std::string& blockName)
    {
        asked.push_back(blockName);
        if (blockName == "nvme0n1")
        {
            return answered(nvmeSmart());
        }
        if (blockName == "sda")
        {
            return answered(ataSmart());
        }
        return DriveSmartRead{.smart = DriveSmart{}, .error = {}}; // a USB stick
    };
    StorageInfo info;
    LinuxStorage::readStorageFacts(root, info, nullptr, reader);
    std::filesystem::remove_all(root);

    EXPECT_EQ(asked, (std::vector<std::string>{"nvme0n1", "sda", "sdb"}));
    ASSERT_EQ(info.disks.size(), 3U);
    EXPECT_TRUE(info.disks[0].health.has_value());
    EXPECT_TRUE(info.disks[1].ataHealth.has_value());
    EXPECT_EQ(info.disks[2].healthUnavailableReason, "udisks2 has no SMART data for this drive");

    // Without a reader (a fixture root in the app), the dash says SMART wasn't read.
    StorageInfo unread;
    write("sys/block/sda/size", "1000\n");
    write("sys/block/sda/device/model", "Disk\n");
    LinuxStorage::readStorageFacts(root, unread, nullptr);
    std::filesystem::remove_all(root);
    ASSERT_EQ(unread.disks.size(), 1U);
    EXPECT_EQ(unread.disks[0].healthUnavailableReason, "SMART status wasn't read");
}

} // namespace
} // namespace Platform
