/// @file test_LinuxOsInfo.cpp
/// @brief Platform::LinuxOsInfo (#1512): os-release parsing, the zone from the /etc/localtime link,
/// the locale choice, btime, and the file facts and container/VM hints read under a fixture root; and
/// Platform::LinuxFirmwareInfo (#1513), the Firmware & board facts from /sys/class/dmi/id under one, and
/// the Memory modules facts (#1515) from the raw DMI table and /proc/meminfo; and Platform::LinuxCommitPaging
/// (#1516), the Commit & paging parsers and facts from /proc and /sys; and Platform::LinuxPlatformSecurity
/// (#1514), the Security parsers and facts from /sys; and Platform::LinuxSensors (#1522), hwmon and
/// thermal-zone sensors.
/// The headers use only the standard library, so these build and run on every platform.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxCommitPaging.h"
#include "Platform/Linux/LinuxFirmwareInfo.h"
#include "Platform/Linux/LinuxOsInfo.h"
#include "Platform/Linux/LinuxPlatformSecurity.h"
#include "Platform/Linux/LinuxSensors.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <optional>
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

/// A raw SMBIOS table, as /sys/firmware/dmi/tables/DMI holds it: one 8 GiB DDR4 SODIMM, then the end.
[[nodiscard]] std::string dmiTable()
{
    std::string table(0x22, '\0');
    const auto put = [&table](std::size_t offset, unsigned value, std::size_t size)
    {
        for (std::size_t i = 0; i < size; ++i)
        {
            table.at(offset + i) = static_cast<char>((value >> (8U * i)) & 0xFFU);
        }
    };
    put(0x00, 17, 1);     // Memory Device
    put(0x01, 0x22, 1);   // length
    put(0x04, 0xFFFE, 2); // no array
    put(0x0C, 0x2000, 2); // 8192 MiB
    put(0x0E, 0x0D, 1);   // SODIMM
    put(0x10, 1, 1);      // locator
    put(0x12, 0x1A, 1);   // DDR4
    put(0x15, 3200, 2);   // speed
    put(0x17, 2, 1);      // manufacturer
    put(0x1A, 3, 1);      // part number
    put(0x20, 3200, 2);   // configured speed
    for (const std::string_view text : {"ChannelA-DIMM0", "Kingston", "KF432C16"})
    {
        table += text;
        table.push_back('\0');
    }
    table.push_back('\0');
    table += std::string{'\x7F', '\x04', '\0', '\0', '\0', '\0'}; // end of table
    return table;
}

TEST(LinuxFirmwareInfoTest, ReadsMemoryModulesAsRoot)
{
    const FixtureRoot root;
    root.write("proc/meminfo", "MemTotal:        8030000 kB\nMemFree:         1000000 kB\n");
    root.write("sys/firmware/dmi/tables/DMI", dmiTable());

    MemoryModulesInfo info;
    LinuxFirmwareInfo::readMemoryModuleFacts(root.path(), info);
    EXPECT_TRUE(info.available);
    EXPECT_TRUE(info.tableRead);
    EXPECT_FALSE(info.tableNeedsAdmin);
    EXPECT_EQ(info.usableBytes, 8030000ULL * 1024);
    EXPECT_EQ(info.slotCount, 1U);
    ASSERT_EQ(info.modules.size(), 1U);
    EXPECT_EQ(info.modules.front().locator, "ChannelA-DIMM0");
    EXPECT_EQ(info.modules.front().type, "DDR4");
    EXPECT_EQ(info.modules.front().formFactor, "SODIMM");
    EXPECT_EQ(info.modules.front().configuredSpeedMts, 3200U);
    EXPECT_EQ(info.modules.front().manufacturer, "Kingston");
    EXPECT_EQ(info.modules.front().partNumber, "KF432C16");
    EXPECT_EQ(info.modules.front().sizeBytes, 8ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(info.installedBytes, 0U); // no figure of its own; the page totals the modules
}

TEST(LinuxFirmwareInfoTest, RootOnlyMemoryTableNeedsAdmin)
{
    const FixtureRoot root;
    root.write("proc/meminfo", "MemTotal: 16000000 kB\n");
    root.write("sys/firmware/dmi/tables/DMI", dmiTable());

    MemoryModulesInfo info;
    LinuxFirmwareInfo::readMemoryModuleFacts(root.path(), info, [](const std::filesystem::path& path) { return path.filename() != "DMI"; });
    EXPECT_TRUE(info.available);
    EXPECT_FALSE(info.tableRead);
    EXPECT_TRUE(info.tableNeedsAdmin);
    EXPECT_TRUE(info.modules.empty());
    EXPECT_EQ(info.installedBytes, 0U);
    EXPECT_EQ(info.usableBytes, 16000000ULL * 1024);

    // No table at all (a container): nothing is root-only.
    const FixtureRoot bare;
    MemoryModulesInfo none;
    LinuxFirmwareInfo::readMemoryModuleFacts(bare.path(), none);
    EXPECT_FALSE(none.tableNeedsAdmin);
    EXPECT_FALSE(none.tableRead);
    EXPECT_EQ(none.usableBytes, 0U);
}

TEST(LinuxFirmwareInfoTest, ParsesMemTotal)
{
    EXPECT_EQ(LinuxFirmwareInfo::parseMemTotalBytes("MemTotal:       1024 kB\n"), 1024ULL * 1024);
    EXPECT_EQ(LinuxFirmwareInfo::parseMemTotalBytes("MemFree: 5 kB\nMemTotal: 2 kB"), 2048U);
    EXPECT_EQ(LinuxFirmwareInfo::parseMemTotalBytes("MemFree: 5 kB\n"), 0U);
    EXPECT_EQ(LinuxFirmwareInfo::parseMemTotalBytes("MemTotal:   \n"), 0U);
    EXPECT_EQ(LinuxFirmwareInfo::parseMemTotalBytes("MemTotal: x kB\n"), 0U);
    EXPECT_EQ(LinuxFirmwareInfo::parseMemTotalBytes(""), 0U);
}

// --- Commit & paging (#1516) ---

/// /proc/meminfo as a 32 GiB desktop writes it, trimmed to the lines around the ones read.
constexpr std::string_view MEMINFO = "MemTotal:       32562356 kB\n"
                                     "MemFree:         8123456 kB\n"
                                     "SwapTotal:      14680052 kB\n"
                                     "CommitLimit:    30961228 kB\n"
                                     "Committed_AS:   14201876 kB\n"
                                     "HugePages_Total:       4\n"
                                     "HugePages_Free:        3\n"
                                     "HugePages_Rsvd:        1\n"
                                     "HugePages_Surp:        0\n"
                                     "Hugepagesize:       2048 kB\n"
                                     "Hugetlb:            8192 kB\n";

/// /proc/swaps with a partition, a zram device and a file whose name has a space (escaped as \040).
constexpr std::string_view SWAPS = "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n"
                                   "/dev/nvme0n1p3                          partition\t8388604\t\t524288\t\t-2\n"
                                   "/dev/zram0                              partition\t4194300\t\t102400\t\t100\n"
                                   "/swap\\040file                           file\t\t2097148\t\t0\t\t-3\n";

TEST(LinuxCommitPagingTest, ParsesTheFiles)
{
    EXPECT_EQ(LinuxCommitPaging::meminfoValue(MEMINFO, "Committed_AS"), 14201876U);
    EXPECT_EQ(LinuxCommitPaging::meminfoValue(MEMINFO, "HugePages_Total"), 4U);
    EXPECT_EQ(LinuxCommitPaging::meminfoValue(MEMINFO, "HugePages"), std::nullopt); // a prefix isn't a key
    EXPECT_EQ(LinuxCommitPaging::meminfoValue("CommitLimit: x kB\n", "CommitLimit"), std::nullopt);
    EXPECT_EQ(LinuxCommitPaging::meminfoValue("CommitLimit:\n", "CommitLimit"), std::nullopt);

    EXPECT_EQ(LinuxCommitPaging::parseOvercommitMode("0\n"), OvercommitMode::Heuristic);
    EXPECT_EQ(LinuxCommitPaging::parseOvercommitMode("1\n"), OvercommitMode::Always);
    EXPECT_EQ(LinuxCommitPaging::parseOvercommitMode("2\n"), OvercommitMode::Strict);
    for (const std::string_view bad : {"3\n", "", "x", "0 1"})
    {
        EXPECT_EQ(LinuxCommitPaging::parseOvercommitMode(bad), OvercommitMode::Unknown) << bad;
    }

    const std::vector<PageFile> swaps = LinuxCommitPaging::parseSwaps(SWAPS);
    ASSERT_EQ(swaps.size(), 3U);
    EXPECT_EQ(swaps[0].path, "/dev/nvme0n1p3");
    EXPECT_EQ(swaps[0].kind, "partition");
    EXPECT_EQ(swaps[0].sizeBytes, 8388604ULL * 1024);
    EXPECT_EQ(swaps[0].usedBytes, 524288ULL * 1024);
    EXPECT_EQ(swaps[0].priority, -2);
    EXPECT_EQ(swaps[1].priority, 100);
    EXPECT_EQ(swaps[2].path, "/swap file");
    EXPECT_EQ(swaps[2].kind, "file");
    EXPECT_TRUE(LinuxCommitPaging::parseSwaps("Filename Type Size Used Priority\n").empty());
    EXPECT_TRUE(LinuxCommitPaging::parseSwaps("/dev/sda2 partition x 0 -2\n/dev/sda3 partition 1\n").empty());
    EXPECT_EQ(LinuxCommitPaging::unescapeSwapPath("/a\\134b\\0"), "/a\\b\\0"); // a short escape stays as written

    const auto zram =
        LinuxCommitPaging::parseZramMmStat("  1073741824   268435456   285212672        0   300000000     1234        0        5\n");
    ASSERT_TRUE(zram.has_value());
    const ZramDevice device = zram.value_or(ZramDevice{});
    EXPECT_EQ(device.originalBytes, 1073741824U);
    EXPECT_EQ(device.compressedBytes, 268435456U);
    EXPECT_EQ(device.memoryUsedBytes, 285212672U);
    EXPECT_FALSE(LinuxCommitPaging::parseZramMmStat("1 2\n").has_value());
    EXPECT_FALSE(LinuxCommitPaging::parseZramMmStat("1 x 3\n").has_value());

    EXPECT_EQ(LinuxCommitPaging::parseSysfsBool("Y\n"), true);
    EXPECT_EQ(LinuxCommitPaging::parseSysfsBool("N\n"), false);
    EXPECT_EQ(LinuxCommitPaging::parseSysfsBool(""), std::nullopt);
    EXPECT_EQ(LinuxCommitPaging::parseBracketedChoice("always [madvise] never\n"), "madvise");
    EXPECT_EQ(LinuxCommitPaging::parseBracketedChoice("always madvise never\n"), "");
    EXPECT_EQ(LinuxCommitPaging::parseBracketedChoice("[always"), "");
}

TEST(LinuxCommitPagingTest, ReadsTheFactsUnderARoot)
{
    const FixtureRoot root;
    root.write("proc/meminfo", MEMINFO);
    root.write("proc/swaps", SWAPS);
    root.write("proc/sys/vm/overcommit_memory", "2\n");
    root.write("sys/block/zram1/mm_stat", "0 0 0 0 0 0 0 0\n");
    root.write("sys/block/zram0/mm_stat", "1073741824 268435456 285212672 0 300000000 1234 0 5\n");
    root.write("sys/block/zram2/mm_stat", "garbage\n"); // left out
    root.write("sys/block/nvme0n1/size", "1000215216\n");
    root.write("sys/module/zswap/parameters/enabled", "N\n");
    root.write("sys/kernel/mm/transparent_hugepage/enabled", "always [madvise] never\n");

    CommitPagingInfo info;
    LinuxCommitPaging::readCommitPagingFacts(root.path(), info);
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Linux);
    EXPECT_EQ(info.committedBytes, 14201876ULL * 1024);
    EXPECT_EQ(info.commitLimitBytes, 30961228ULL * 1024);
    EXPECT_EQ(info.overcommit, OvercommitMode::Strict);
    EXPECT_TRUE(info.pageFilesRead);
    EXPECT_EQ(info.pageFiles.size(), 3U);
    EXPECT_TRUE(info.zramRead);
    ASSERT_EQ(info.zram.size(), 2U);
    EXPECT_EQ(info.zram[0].name, "zram0"); // sorted by name
    EXPECT_EQ(info.zram[0].compressedBytes, 268435456U);
    EXPECT_EQ(info.zram[1].name, "zram1");
    EXPECT_EQ(info.zswapEnabled, false);
    EXPECT_TRUE(info.hugePagesRead);
    EXPECT_EQ(info.hugePagesTotal, 4U);
    EXPECT_EQ(info.hugePagesFree, 3U);
    EXPECT_EQ(info.hugePagesReserved, 1U);
    EXPECT_EQ(info.hugePagesSurplus, 0U);
    EXPECT_EQ(info.hugePageSizeBytes, 2048ULL * 1024);
    EXPECT_EQ(info.transparentHugePages, "madvise");
}

TEST(LinuxCommitPagingTest, NothingReadableLeavesEverythingUnknown)
{
    const FixtureRoot root; // none of the files exist
    CommitPagingInfo info;
    LinuxCommitPaging::readCommitPagingFacts(root.path(), info);
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.committedBytes, 0U);
    EXPECT_EQ(info.commitLimitBytes, 0U);
    EXPECT_EQ(info.overcommit, OvercommitMode::Unknown);
    EXPECT_FALSE(info.pageFilesRead);
    EXPECT_FALSE(info.zramRead);
    EXPECT_FALSE(info.zswapEnabled.has_value());
    EXPECT_FALSE(info.hugePagesRead);
    EXPECT_TRUE(info.transparentHugePages.empty());
}

TEST(LinuxPlatformSecurityTest, ParsesTheFiles)
{
    using namespace std::string_view_literals;
    // Four attribute bytes, then the value.
    EXPECT_EQ(LinuxPlatformSecurity::parseSecureBootVariable("\x06\x00\x00\x00\x01"sv), std::optional<bool>(true));
    EXPECT_EQ(LinuxPlatformSecurity::parseSecureBootVariable("\x06\x00\x00\x00\x00"sv), std::optional<bool>(false));
    EXPECT_FALSE(LinuxPlatformSecurity::parseSecureBootVariable("\x06\x00\x00\x00"sv).has_value());
    EXPECT_EQ(LinuxPlatformSecurity::parseLsmList("lockdown,capability,landlock,yama,apparmor\n"),
              (std::vector<std::string>{"lockdown", "capability", "landlock", "yama", "apparmor"}));
    EXPECT_TRUE(LinuxPlatformSecurity::parseLsmList("").empty());
    EXPECT_EQ(LinuxPlatformSecurity::parseSelinuxEnforce("1"), std::optional<bool>(true));
    EXPECT_EQ(LinuxPlatformSecurity::parseSelinuxEnforce("0"), std::optional<bool>(false));
    EXPECT_FALSE(LinuxPlatformSecurity::parseSelinuxEnforce("").has_value());
}

TEST(LinuxPlatformSecurityTest, ReadsTheFactsUnderARoot)
{
    using namespace std::string_view_literals;
    const FixtureRoot root;
    root.write(std::format("sys/firmware/efi/efivars/{}", LinuxPlatformSecurity::SECURE_BOOT_VARIABLE), "\x06\x00\x00\x00\x01"sv);
    root.write("sys/class/tpm/tpm0/tpm_version_major", "2\n");
    root.write("sys/kernel/security/lsm", "lockdown,capability,yama,apparmor");
    root.write("sys/kernel/security/lockdown", "none [integrity] confidentiality\n");
    root.write("sys/module/apparmor/parameters/enabled", "Y\n");
    root.write("sys/devices/system/cpu/vulnerabilities/spectre_v2", "Mitigation: Enhanced / Automatic IBRS\n");
    root.write("sys/devices/system/cpu/vulnerabilities/meltdown", "Not affected\n");
    root.write("sys/devices/system/cpu/vulnerabilities/empty", ""); // left out

    PlatformSecurityInfo info;
    LinuxPlatformSecurity::readPlatformSecurityFacts(root.path(), info);
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.secureBoot, SecurityFeatureState::On);
    EXPECT_EQ(info.tpm, SecurityFeatureState::On);
    EXPECT_EQ(info.tpmVersionMajor, 2U);
    EXPECT_TRUE(info.lsmRead);
    EXPECT_EQ(info.lsms.size(), 4U);
    EXPECT_FALSE(info.selinuxEnforcing.has_value());
    EXPECT_EQ(info.apparmorEnabled, std::optional<bool>(true));
    EXPECT_EQ(info.lockdown, "integrity");
    EXPECT_TRUE(info.vulnerabilitiesRead);
    ASSERT_EQ(info.vulnerabilities.size(), 2U);
    EXPECT_EQ(info.vulnerabilities[0].name, "meltdown"); // sorted by name
    EXPECT_EQ(info.vulnerabilities[0].status, "Not affected");
    EXPECT_EQ(info.vulnerabilities[1].name, "spectre_v2");
}

TEST(LinuxPlatformSecurityTest, SecureBootOffLegacyBiosAndAMissingVariable)
{
    using namespace std::string_view_literals;
    {
        const FixtureRoot root;
        root.write(std::format("sys/firmware/efi/efivars/{}", LinuxPlatformSecurity::SECURE_BOOT_VARIABLE), "\x06\x00\x00\x00\x00"sv);
        EXPECT_EQ(LinuxPlatformSecurity::readSecureBoot(root.path()), SecurityFeatureState::Off);
    }
    {
        const FixtureRoot root; // no /sys/firmware/efi: booted from a legacy BIOS
        EXPECT_EQ(LinuxPlatformSecurity::readSecureBoot(root.path()), SecurityFeatureState::NotSupported);
    }
    {
        const FixtureRoot root; // efivarfs mounted, but the firmware has no SecureBoot variable
        root.write("sys/firmware/efi/efivars/BootOrder-8be4df61-93ca-11d2-aa0d-00e098032b8c", "\x07\x00\x00\x00\x01\x00"sv);
        EXPECT_EQ(LinuxPlatformSecurity::readSecureBoot(root.path()), SecurityFeatureState::NotSupported);
    }
    {
        const FixtureRoot root; // UEFI, but efivarfs isn't mounted: can't tell
        std::filesystem::create_directories(root.path() / "sys/firmware/efi");
        EXPECT_EQ(LinuxPlatformSecurity::readSecureBoot(root.path()), SecurityFeatureState::Unknown);
    }
    {
        const FixtureRoot root; // the older sysfs interface: the value alone
        root.write(std::format("sys/firmware/efi/vars/{}/data", LinuxPlatformSecurity::SECURE_BOOT_VARIABLE), "\x01"sv);
        EXPECT_EQ(LinuxPlatformSecurity::readSecureBoot(root.path()), SecurityFeatureState::On);
    }
}

TEST(LinuxPlatformSecurityTest, NothingReadableLeavesEverythingUnknown)
{
    const FixtureRoot root; // none of the files exist, not even /sys/class
    PlatformSecurityInfo info;
    LinuxPlatformSecurity::readPlatformSecurityFacts(root.path(), info);
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.secureBoot, SecurityFeatureState::NotSupported);
    EXPECT_EQ(info.tpm, SecurityFeatureState::Unknown);
    EXPECT_FALSE(info.lsmRead);
    EXPECT_FALSE(info.selinuxEnforcing.has_value());
    EXPECT_FALSE(info.apparmorEnabled.has_value());
    EXPECT_TRUE(info.lockdown.empty());
    EXPECT_FALSE(info.vulnerabilitiesRead);

    std::filesystem::create_directories(root.path() / "sys/class"); // sysfs, but no TPM
    LinuxPlatformSecurity::readPlatformSecurityFacts(root.path(), info);
    EXPECT_EQ(info.tpm, SecurityFeatureState::NotSupported);
}

TEST(LinuxSensorsTest, ParsesValuesAndChannels)
{
    EXPECT_EQ(LinuxSensors::parseSysfsInteger("45000\n"), std::optional<std::int64_t>(45000));
    EXPECT_EQ(LinuxSensors::parseSysfsInteger("-5000"), std::optional<std::int64_t>(-5000));
    EXPECT_FALSE(LinuxSensors::parseSysfsInteger("").has_value());
    EXPECT_FALSE(LinuxSensors::parseSysfsInteger("12a").has_value());
    const LinuxSensors::InputFamily& temp = LinuxSensors::INPUT_FAMILIES[0];
    EXPECT_EQ(LinuxSensors::inputChannel("temp3_input", temp), std::optional<unsigned>(3));
    EXPECT_FALSE(LinuxSensors::inputChannel("temp3_label", temp).has_value());
    EXPECT_FALSE(LinuxSensors::inputChannel("temp_input", temp).has_value());
    EXPECT_FALSE(LinuxSensors::inputChannel("tempX_input", temp).has_value());
}

TEST(LinuxSensorsTest, ReadsChipsAndThermalZonesUnderARoot)
{
    const FixtureRoot root;
    // A CPU package sensor with thresholds, and a core without.
    root.write("sys/class/hwmon/hwmon2/name", "coretemp\n");
    root.write("sys/class/hwmon/hwmon2/temp1_input", "52000\n");
    root.write("sys/class/hwmon/hwmon2/temp1_label", "Package id 0\n");
    root.write("sys/class/hwmon/hwmon2/temp1_max", "100000\n");
    root.write("sys/class/hwmon/hwmon2/temp1_crit", "105000\n");
    root.write("sys/class/hwmon/hwmon2/temp2_input", "48000\n");
    // A board chip: a fan, a voltage, a disconnected fan (unreadable input), and power in both forms.
    root.write("sys/class/hwmon/hwmon0/name", "nct6798\n");
    root.write("sys/class/hwmon/hwmon0/fan1_input", "1180\n");
    root.write("sys/class/hwmon/hwmon0/fan2_input", "");
    root.write("sys/class/hwmon/hwmon0/in0_input", "12180\n");
    root.write("sys/class/hwmon/hwmon0/in0_label", "+12V\n");
    root.write("sys/class/hwmon/hwmon0/power1_input", "15200000\n");
    root.write("sys/class/hwmon/hwmon0/power1_average", "15000000\n");
    root.write("sys/class/hwmon/hwmon0/curr1_input", "1200\n");
    // Two NVMe drives, numbered in hwmon order, and an AC adapter with nothing to read.
    root.write("sys/class/hwmon/hwmon3/name", "nvme\n");
    root.write("sys/class/hwmon/hwmon3/temp1_input", "38850\n");
    root.write("sys/class/hwmon/hwmon4/name", "nvme\n");
    root.write("sys/class/hwmon/hwmon4/temp1_input", "41850\n");
    root.write("sys/class/hwmon/hwmon1/name", "AC\n");
    // Thermal zones: acpitz is also hwmon5, so only x86_pkg_temp is added.
    root.write("sys/class/hwmon/hwmon5/name", "acpitz\n");
    root.write("sys/class/hwmon/hwmon5/temp1_input", "27800\n");
    root.write("sys/class/thermal/thermal_zone0/type", "acpitz\n");
    root.write("sys/class/thermal/thermal_zone0/temp", "27800\n");
    root.write("sys/class/thermal/thermal_zone1/type", "x86_pkg_temp\n");
    root.write("sys/class/thermal/thermal_zone1/temp", "52000\n");
    root.write("sys/class/thermal/cooling_device0/type", "Processor\n");

    SensorsInfo info;
    LinuxSensors::readSensorFacts(root.path(), info);
    EXPECT_TRUE(info.available);
    EXPECT_TRUE(info.listed);
    ASSERT_EQ(info.devices.size(), 6U);
    EXPECT_EQ(info.devices[0].name, "nct6798"); // hwmon0; hwmon1 (AC) has nothing to read
    ASSERT_EQ(info.devices[0].readings.size(), 4U);
    EXPECT_EQ(info.devices[0].readings[0].kind, SensorKind::Fan); // kind order: temperature, fan, voltage, current, power
    EXPECT_EQ(info.devices[0].readings[0].label, "fan1");
    EXPECT_DOUBLE_EQ(info.devices[0].readings[0].value, 1180.0);
    EXPECT_EQ(info.devices[0].readings[1].label, "+12V");
    EXPECT_DOUBLE_EQ(info.devices[0].readings[1].value, 12.18);
    EXPECT_EQ(info.devices[0].readings[2].kind, SensorKind::Current);
    EXPECT_DOUBLE_EQ(info.devices[0].readings[2].value, 1.2);
    EXPECT_EQ(info.devices[0].readings[3].kind, SensorKind::Power);
    EXPECT_DOUBLE_EQ(info.devices[0].readings[3].value, 15.2); // _input preferred over _average

    EXPECT_EQ(info.devices[1].name, "coretemp");
    ASSERT_EQ(info.devices[1].readings.size(), 2U);
    EXPECT_EQ(info.devices[1].readings[0].label, "Package id 0");
    EXPECT_DOUBLE_EQ(info.devices[1].readings[0].value, 52.0);
    EXPECT_EQ(info.devices[1].readings[0].high, std::optional<double>(100.0));
    EXPECT_EQ(info.devices[1].readings[0].critical, std::optional<double>(105.0));
    EXPECT_EQ(info.devices[1].readings[1].label, "temp2");
    EXPECT_FALSE(info.devices[1].readings[1].high.has_value());

    EXPECT_EQ(info.devices[2].name, "nvme");
    EXPECT_EQ(info.devices[3].name, "nvme #2");
    EXPECT_EQ(info.devices[4].name, "acpitz");
    EXPECT_EQ(info.devices[5].name, "Thermal zones");
    ASSERT_EQ(info.devices[5].readings.size(), 1U);
    EXPECT_EQ(info.devices[5].readings[0].label, "x86_pkg_temp");
}

TEST(LinuxSensorsTest, NoSensorsAndNoSysfs)
{
    {
        const FixtureRoot root;
        std::filesystem::create_directories(root.path() / "sys/class/hwmon"); // listed, but empty (a VM)
        SensorsInfo info;
        LinuxSensors::readSensorFacts(root.path(), info);
        EXPECT_TRUE(info.listed);
        EXPECT_TRUE(info.devices.empty());
    }
    {
        const FixtureRoot root; // no /sys at all
        SensorsInfo info;
        LinuxSensors::readSensorFacts(root.path(), info);
        EXPECT_TRUE(info.available);
        EXPECT_FALSE(info.listed);
        EXPECT_TRUE(info.devices.empty());
    }
}

} // namespace
} // namespace Platform::LinuxOsInfo
