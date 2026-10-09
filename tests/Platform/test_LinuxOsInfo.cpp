/// @file test_LinuxOsInfo.cpp
/// @brief Platform::LinuxOsInfo (#1512): os-release parsing, the zone from the /etc/localtime link,
/// the locale choice, btime, and the file facts and container/VM hints read under a fixture root; and
/// Platform::LinuxFirmwareInfo (#1513), the Firmware & board facts from /sys/class/dmi/id under one.
/// Both headers use only the standard library, so these build and run on every platform.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxFirmwareInfo.h"
#include "Platform/Linux/LinuxOsInfo.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform::LinuxOsInfo
{
namespace
{

/// A fresh fixture root under the temp directory, removed (best effort) when the test ends.
class FixtureRoot
{
  public:
    FixtureRoot()
        : m_Path(std::filesystem::temp_directory_path() /
                 std::format("ts_osinfo_{}_{}", s_Counter.fetch_add(1), std::chrono::steady_clock::now().time_since_epoch().count()))
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

    void write(std::string_view relative, std::string_view text) const
    {
        const auto file = m_Path / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out << text;
    }

  private:
    static inline std::atomic<unsigned> s_Counter{0};
    std::filesystem::path m_Path;
};

TEST(LinuxOsInfoTest, ParsesOsRelease)
{
    const OsRelease release = parseOsRelease("# comment\n"
                                             "NAME=\"Ubuntu\"\r\n"
                                             "VERSION_ID=\"24.04\"\n"
                                             "PRETTY_NAME=\"Ubuntu 24.04.1 LTS\"\n"
                                             "ID=ubuntu\n"
                                             "\n"
                                             "garbage line\n");
    EXPECT_EQ(release.prettyName, "Ubuntu 24.04.1 LTS");
    EXPECT_EQ(release.versionId, "24.04");

    // NAME stands in for a missing PRETTY_NAME; unquoted and single-quoted values; escapes undone.
    const OsRelease bare = parseOsRelease("NAME='Arch Linux'\nVERSION_ID=rolling");
    EXPECT_EQ(bare.prettyName, "Arch Linux");
    EXPECT_EQ(bare.versionId, "rolling");
    EXPECT_EQ(unquoteOsReleaseValue(R"("say \"hi\" \\ \$x")"), R"(say "hi" \ $x)");
    EXPECT_EQ(unquoteOsReleaseValue(R"(a\\)"), R"(a\)"); // an escaped backslash at the end
    EXPECT_EQ(unquoteOsReleaseValue(R"(a\)"), R"(a\)");  // a trailing backslash is kept
    EXPECT_EQ(unquoteOsReleaseValue(R"(\\\\)"), R"(\\)");
    EXPECT_EQ(unquoteOsReleaseValue(""), "");
    EXPECT_EQ(parseOsRelease("").prettyName, "");
}

TEST(LinuxOsInfoTest, TimeZoneFromLocaltimeTarget)
{
    EXPECT_EQ(timeZoneFromLocaltimeTarget("/usr/share/zoneinfo/America/Los_Angeles"), "America/Los_Angeles");
    EXPECT_EQ(timeZoneFromLocaltimeTarget("../usr/share/zoneinfo/Europe/Berlin"), "Europe/Berlin");
    EXPECT_EQ(timeZoneFromLocaltimeTarget("/usr/share/zoneinfo/posix/Asia/Tokyo"), "Asia/Tokyo");
    EXPECT_EQ(timeZoneFromLocaltimeTarget("/usr/share/zoneinfo/UTC"), "UTC");
    EXPECT_EQ(timeZoneFromLocaltimeTarget("/etc/some-copy"), "");
}

TEST(LinuxOsInfoTest, ChoosesLocaleLikeSetlocale)
{
    EXPECT_EQ(chooseLocale("de_DE.UTF-8", "en_US.UTF-8"), "de_DE.UTF-8");
    EXPECT_EQ(chooseLocale("", "en_US.UTF-8"), "en_US.UTF-8");
    EXPECT_EQ(chooseLocale(nullptr, nullptr), "C");
}

TEST(LinuxOsInfoTest, ParsesBootTime)
{
    EXPECT_EQ(parseBootTime("cpu  1 2 3\ncpu0 1 2 3\nintr 5\nbtime 1704164645\nprocesses 9\n"), 1704164645U);
    EXPECT_EQ(parseBootTime("cpu  1 2 3\n"), 0U);
}

TEST(LinuxOsInfoTest, ReadsFileFactsUnderTheRoot)
{
    const FixtureRoot root;
    root.write("usr/lib/os-release", "PRETTY_NAME=\"Fedora Linux 41 (Workstation Edition)\"\nVERSION_ID=41\n"); // the fallback
    root.write("proc/1/comm", "systemd\n");
    root.write("proc/stat", "cpu  1 2 3\nbtime 1700000000\n");
    root.write("etc/timezone", "Europe/Paris\n"); // no localtime link here
    root.write("sys/class/dmi/id/sys_vendor", "QEMU\n");
    root.write("sys/class/dmi/id/product_name", "Standard PC (Q35 + ICH9, 2009)\n");

    OsInfo info;
    readFileFacts(root.path(), info);
    EXPECT_EQ(info.name, "Fedora Linux 41 (Workstation Edition)");
    EXPECT_EQ(info.version, "41");
    EXPECT_EQ(info.initSystem, "systemd");
    EXPECT_EQ(info.bootUnixSeconds, 1700000000U);
    EXPECT_EQ(info.timeZone, "Europe/Paris");
    EXPECT_EQ(info.virtualization, "Virtual machine (QEMU)");

    // /etc/os-release wins over the fallback; a localtime link, where the file system allows one.
    root.write("etc/os-release", "PRETTY_NAME=\"Debian GNU/Linux 13 (trixie)\"\n");
    std::error_code ec;
    std::filesystem::create_symlink("../usr/share/zoneinfo/Asia/Kolkata", root.path() / "etc/localtime", ec);
    OsInfo again;
    readFileFacts(root.path(), again);
    EXPECT_EQ(again.name, "Debian GNU/Linux 13 (trixie)");
    EXPECT_EQ(again.timeZone, ec ? "Europe/Paris" : "Asia/Kolkata");
}

TEST(LinuxOsInfoTest, VirtualizationHints)
{
    const FixtureRoot root;
    EXPECT_EQ(virtualizationHint(root.path()), "None detected");
    root.write("sys/class/dmi/id/sys_vendor", "Microsoft Corporation");
    root.write("sys/class/dmi/id/product_name", "Virtual Machine");
    EXPECT_EQ(virtualizationHint(root.path()), "Virtual machine (Hyper-V)");
    root.write(".dockerenv", "");
    EXPECT_EQ(virtualizationHint(root.path()), "Container (Docker)");
    root.write("run/.containerenv", "engine=\"podman\"\n");
    EXPECT_EQ(virtualizationHint(root.path()), "Container (Podman)");
}

void writeDmi(const FixtureRoot& root)
{
    const std::string dmi = "sys/class/dmi/id/";
    root.write(dmi + "sys_vendor", "LENOVO\n");
    root.write(dmi + "product_name", "21KC\n");
    root.write(dmi + "product_version", "ThinkPad T14 Gen 5\n");
    root.write(dmi + "product_family", "ThinkPad T14 Gen 5\n");
    root.write(dmi + "product_sku", "LENOVO_MT_21KC\n");
    root.write(dmi + "bios_vendor", "LENOVO\n");
    root.write(dmi + "bios_version", "R2LET30W (1.11 )\n");
    root.write(dmi + "bios_date", "05/14/2024\n");
    root.write(dmi + "ec_firmware_release", "1.7\n");
    root.write(dmi + "board_vendor", "LENOVO\n");
    root.write(dmi + "board_name", "21KCCTO1WW\n");
    root.write(dmi + "board_version", "SDK0T76530 WIN\n");
    root.write(dmi + "chassis_type", "10\n");
    root.write(dmi + "chassis_vendor", "LENOVO\n");
    // Root-only (0400) on a real system.
    root.write(dmi + "product_serial", "PF4XXXXX\n");
    root.write(dmi + "product_uuid", "0c1f2a3b-0000-0000-0000-000000000000\n");
    root.write(dmi + "board_serial", "L1HF4XXXXX\n");
}

TEST(LinuxFirmwareInfoTest, ReadsDmiAsRoot)
{
    const FixtureRoot root;
    writeDmi(root);
    root.write("sys/firmware/efi/fw_platform_size", "64\n");
    std::vector<char> entryPoint(0x18, '\0');
    const std::string anchor = "_SM3_";
    std::ranges::copy(anchor, entryPoint.begin());
    entryPoint.at(7) = 3;
    entryPoint.at(8) = 6;
    root.write("sys/firmware/dmi/tables/smbios_entry_point", std::string_view(entryPoint.data(), entryPoint.size()));

    FirmwareInfo info;
    LinuxFirmwareInfo::readFirmwareFacts(root.path(), info);
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.systemManufacturer, "LENOVO");
    EXPECT_EQ(info.systemModel, "21KC");
    EXPECT_EQ(info.systemVersion, "ThinkPad T14 Gen 5");
    EXPECT_EQ(info.systemSku, "LENOVO_MT_21KC");
    EXPECT_EQ(info.biosVersion, "R2LET30W (1.11 )");
    EXPECT_EQ(info.biosReleaseDate, "05/14/2024");
    EXPECT_EQ(info.embeddedControllerVersion, "1.7");
    EXPECT_EQ(info.boardProduct, "21KCCTO1WW");
    EXPECT_EQ(info.chassisType, "Notebook");
    EXPECT_EQ(info.platformRole, "Mobile");
    EXPECT_EQ(info.firmwareMode, FirmwareMode::Uefi);
    EXPECT_EQ(info.smbiosVersion, "3.6");
    EXPECT_EQ(info.systemSerial, "PF4XXXXX");
    EXPECT_EQ(info.boardSerial, "L1HF4XXXXX");
    EXPECT_FALSE(info.identifiersNeedAdmin);
    EXPECT_FALSE(info.smbiosVersionNeedsAdmin);
}

TEST(LinuxFirmwareInfoTest, RootOnlyFilesNeedAdmin)
{
    const FixtureRoot root;
    writeDmi(root);
    root.write("sys/firmware/acpi/tables/DSDT", ""); // /sys/firmware without efi: legacy BIOS
    root.write("sys/firmware/dmi/tables/smbios_entry_point", "_SM3_");
    const auto unprivileged = [](const std::filesystem::path& path)
    {
        const std::string name = path.filename().string();
        return name != "product_serial" && name != "product_uuid" && name != "board_serial" && name != "smbios_entry_point";
    };

    FirmwareInfo info;
    LinuxFirmwareInfo::readFirmwareFacts(root.path(), info, unprivileged);
    EXPECT_EQ(info.systemManufacturer, "LENOVO");
    EXPECT_EQ(info.systemSerial, "");
    EXPECT_EQ(info.systemUuid, "");
    EXPECT_EQ(info.boardSerial, "");
    EXPECT_TRUE(info.identifiersNeedAdmin);
    EXPECT_EQ(info.smbiosVersion, "");
    EXPECT_TRUE(info.smbiosVersionNeedsAdmin);
    EXPECT_EQ(info.firmwareMode, FirmwareMode::Legacy);
}

TEST(LinuxFirmwareInfoTest, NoDmiAtAll)
{
    // A container or a board without DMI: nothing is there, so nothing is root-only either.
    const FixtureRoot root;
    FirmwareInfo info;
    LinuxFirmwareInfo::readFirmwareFacts(root.path(), info, [](const std::filesystem::path&) { return false; });
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.systemManufacturer, "");
    EXPECT_EQ(info.chassisType, "");
    EXPECT_EQ(info.firmwareMode, FirmwareMode::Unknown);
    EXPECT_FALSE(info.identifiersNeedAdmin);
    EXPECT_FALSE(info.smbiosVersionNeedsAdmin);

    EXPECT_EQ(LinuxFirmwareInfo::parseChassisType("3"), 3);
    EXPECT_EQ(LinuxFirmwareInfo::parseChassisType("x"), 0);
    EXPECT_EQ(LinuxFirmwareInfo::parseChassisType("300"), 0);
}

} // namespace
} // namespace Platform::LinuxOsInfo
