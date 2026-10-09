/// @file test_LinuxOsInfo.cpp
/// @brief Platform::LinuxOsInfo (#1512): os-release parsing, the zone from the /etc/localtime link,
/// the locale choice, btime, and the file facts and container/VM hints read under a fixture root; and
/// Platform::LinuxFirmwareInfo (#1513), the Firmware & board facts from /sys/class/dmi/id under one, and
/// the Memory modules facts (#1515) from the raw DMI table and /proc/meminfo; and Platform::LinuxCommitPaging
/// (#1516), the Commit & paging parsers and facts from /proc and /sys; and Platform::LinuxStorage (#1517),
/// the mountinfo parser and the volume choice, and the disks and volumes under a fixture root; and
/// Platform::LinuxPlatformSecurity (#1514), the Security parsers and facts from /sys.
/// The headers use only the standard library, so these build and run on every platform.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxCommitPaging.h"
#include "Platform/Linux/LinuxFirmwareInfo.h"
#include "Platform/Linux/LinuxOsInfo.h"
#include "Platform/Linux/LinuxPlatformSecurity.h"
#include "Platform/Linux/LinuxStorage.h"

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

// --- Storage (#1517) ---

/// /proc/self/mountinfo from a desktop, trimmed: the root and EFI partitions, kernel and memory file
/// systems, a snap, an NTFS disk whose mount point has a space (\040), a bind mount of a directory on the
/// root file system, an NFS share, and a disk whose bind mount is listed before its own mount.
constexpr std::string_view MOUNTINFO = "22 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw,errors=remount-ro\n"
                                       "23 22 0:21 / /proc rw,nosuid,nodev,noexec,relatime shared:12 - proc proc rw\n"
                                       "24 22 0:22 / /sys rw,nosuid,nodev,noexec,relatime shared:2 - sysfs sysfs rw\n"
                                       "25 22 0:5 / /dev rw,nosuid,relatime shared:3 - devtmpfs udev rw,size=16243500k\n"
                                       "26 25 0:23 / /dev/shm rw,nosuid,nodev shared:4 - tmpfs tmpfs rw\n"
                                       "27 22 0:24 / /run rw,nosuid,nodev shared:5 - tmpfs tmpfs rw,size=3254180k\n"
                                       "28 22 0:25 / /tmp rw,nosuid,nodev shared:6 - tmpfs tmpfs rw\n"
                                       "29 22 259:1 / /boot/efi rw,relatime shared:7 - vfat /dev/nvme0n1p1 rw,fmask=0077\n"
                                       "30 24 0:26 / /sys/fs/cgroup rw,nosuid shared:8 - cgroup2 cgroup2 rw\n"
                                       "31 22 7:0 / /snap/core22/1380 ro,nodev,relatime shared:9 - squashfs /dev/loop0 ro\n"
                                       "32 22 8:17 / /mnt/My\\040Data rw,relatime shared:10 - ntfs3 /dev/sdb1 rw\n"
                                       "33 22 259:2 /srv/data /srv/bind rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw\n"
                                       "34 22 0:40 / /mnt/nas rw,relatime shared:11 master:3 - nfs4 nas:/export rw,vers=4.2\n"
                                       "35 27 0:41 / /run/user/1000/doc rw,nosuid shared:13 - fuse.portal portal rw\n"
                                       "36 22 0:42 / /var/lib/docker/overlay2/abc/merged rw shared:14 - overlay overlay rw\n"
                                       "37 22 8:33 /sub /early rw shared:15 - ext4 /dev/sdc1 rw\n"
                                       "38 22 8:33 / /data rw shared:16 - ext4 /dev/sdc1 rw\n"
                                       "garbage\n"
                                       "39 22 8:49 / /no-separator rw ext4 /dev/sdd1 rw\n";

TEST(LinuxStorageTest, ParsesMountInfo)
{
    const std::vector<LinuxStorage::MountEntry> entries = LinuxStorage::parseMountInfo(MOUNTINFO);
    ASSERT_EQ(entries.size(), 17U); // the last two lines are skipped
    EXPECT_EQ(entries[0].device, "259:2");
    EXPECT_EQ(entries[0].root, "/");
    EXPECT_EQ(entries[0].mountPoint, "/");
    EXPECT_EQ(entries[0].fileSystem, "ext4");
    EXPECT_EQ(entries[0].source, "/dev/nvme0n1p2");
    EXPECT_EQ(entries[10].mountPoint, "/mnt/My Data"); // \040 undone
    EXPECT_EQ(entries[11].root, "/srv/data");
    EXPECT_EQ(entries[12].fileSystem, "nfs4"); // two optional fields before the separator
    EXPECT_EQ(entries[12].source, "nas:/export");
    EXPECT_TRUE(LinuxStorage::parseMountInfo("").empty());
    EXPECT_TRUE(LinuxStorage::parseMountInfo("1 2 3:4 / / rw - ext4\n").empty()); // no source
    EXPECT_TRUE(LinuxStorage::parseMountInfo("- - - - - - - -\n").empty());       // a separator with one field after it
}

TEST(LinuxStorageTest, ChoosesRealVolumesOncePerDevice)
{
    const std::vector<LinuxStorage::MountEntry> volumes = LinuxStorage::selectVolumes(LinuxStorage::parseMountInfo(MOUNTINFO));
    std::vector<std::string> mountPoints;
    mountPoints.reserve(volumes.size());
    for (const LinuxStorage::MountEntry& volume : volumes)
    {
        mountPoints.push_back(volume.mountPoint);
    }
    // No proc, sysfs, devtmpfs, /dev/shm, /run, cgroup, the snap, the portal or overlay; /tmp is kept;
    // the bind mount of a directory on / is dropped, and /data wins over its earlier bind mount /early.
    EXPECT_EQ(mountPoints, (std::vector<std::string>{"/", "/tmp", "/boot/efi", "/mnt/My Data", "/mnt/nas", "/data"}));

    EXPECT_TRUE(LinuxStorage::isNetworkFileSystem("nfs4"));
    EXPECT_TRUE(LinuxStorage::isNetworkFileSystem("fuse.sshfs"));
    EXPECT_FALSE(LinuxStorage::isNetworkFileSystem("ext4"));
    EXPECT_FALSE(LinuxStorage::isPseudoFileSystem("fuseblk", "/media/usb")); // NTFS through FUSE is a disk
    EXPECT_FALSE(LinuxStorage::isPseudoFileSystem("fuse.sshfs", "/mnt/remote"));
    EXPECT_TRUE(LinuxStorage::isPseudoFileSystem("fuse.gvfsd-fuse", "/run/user/1000/gvfs"));
    EXPECT_TRUE(LinuxStorage::isPseudoFileSystem("ext4", "/sys/kernel/x"));
}

TEST(LinuxStorageTest, ParsesUdevAndBusNames)
{
    constexpr std::string_view UDEV = "S:disk/by-id/ata-WDC\nE:ID_BUS=ata\nE:ID_SERIAL_SHORT=WD-WCC4M1234567\nE:ID_FS_LABEL=My Data\n";
    EXPECT_EQ(LinuxStorage::udevProperty(UDEV, "ID_BUS"), "ata");
    EXPECT_EQ(LinuxStorage::udevProperty(UDEV, "ID_FS_LABEL"), "My Data");
    EXPECT_EQ(LinuxStorage::udevProperty(UDEV, "ID_SERIAL"), ""); // a prefix of a key isn't the key
    EXPECT_EQ(LinuxStorage::udevProperty("E:ID_BUS\n", "ID_BUS"), "");
    EXPECT_EQ(LinuxStorage::diskBus("nvme0n1", ""), "NVMe");
    EXPECT_EQ(LinuxStorage::diskBus("sda", "ata"), "SATA");
    EXPECT_EQ(LinuxStorage::diskBus("sdb", "usb"), "USB");
    EXPECT_EQ(LinuxStorage::diskBus("mmcblk0", ""), "MMC");
    EXPECT_EQ(LinuxStorage::diskBus("sdc", ""), "");
    EXPECT_EQ(LinuxStorage::trimmed("  WDC WD20EZRZ   \n"), "WDC WD20EZRZ");
    EXPECT_EQ(LinuxStorage::trimmed(" \n"), "");
}

/// The mount points fakeSizer() was asked about.
std::vector<std::string>& sizedMounts()
{
    static std::vector<std::string> mounts;
    return mounts;
}

/// A fake statvfs(): 100 GiB with 25 GiB free, except /boot/efi, which fails.
bool fakeSizer(const std::string& mountPoint, std::uint64_t& sizeBytes, std::uint64_t& freeBytes)
{
    constexpr std::uint64_t GIB = std::uint64_t{1024} * 1024 * 1024;
    sizedMounts().push_back(mountPoint);
    if (mountPoint == "/boot/efi")
    {
        return false;
    }
    sizeBytes = 100 * GIB;
    freeBytes = 25 * GIB;
    return true;
}

TEST(LinuxStorageTest, ReadsDisksAndVolumesUnderARoot)
{
    const FixtureRoot root;
    root.write("proc/self/mountinfo", MOUNTINFO);
    root.write("sys/block/nvme0n1/size", "1953525168\n");
    root.write("sys/block/nvme0n1/dev", "259:0\n");
    root.write("sys/block/nvme0n1/queue/rotational", "0\n");
    root.write("sys/block/nvme0n1/device/model", "Samsung SSD 980 PRO 1TB                 \n");
    root.write("sys/block/nvme0n1/device/firmware_rev", "5B2QGXA7\n");
    root.write("sys/block/nvme0n1/device/serial", "S5GXNX0R123456      \n");
    root.write("sys/block/nvme0n1/device/hwmon2/temp1_input", "40850\n");
    root.write("sys/block/sda/size", "3907029168\n");
    root.write("sys/block/sda/dev", "8:0\n");
    root.write("sys/block/sda/queue/rotational", "1\n");
    root.write("sys/block/sda/device/model", "WDC WD20EZRZ-00Z\n");
    root.write("sys/block/sda/device/rev", "0A80\n");
    root.write("sys/block/sda/device/hwmon/hwmon4/temp1_input", "35000\n");
    root.write("run/udev/data/b8:0", "E:ID_BUS=ata\nE:ID_SERIAL_SHORT=WD-WCC4M1234567\n");
    root.write("run/udev/data/b259:2", "E:ID_FS_LABEL=root\n");
    root.write("sys/block/loop0/size", "2048\n");    // virtual: left out
    root.write("sys/block/zram0/size", "8388608\n"); // virtual
    root.write("sys/block/dm-0/size", "1000\n");     // device-mapper
    root.write("sys/block/sr0/size", "0\n");         // an optical drive with no disc
    root.write("sys/block/sr0/device/model", "DVD-RW\n");
    root.write("sys/block/xvda/size", "1000\n"); // no device link: not a physical disk

    sizedMounts().clear();
    StorageInfo info;
    LinuxStorage::readStorageFacts(root.path(), info, &fakeSizer);
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Linux);
    EXPECT_TRUE(info.disksRead);
    ASSERT_EQ(info.disks.size(), 2U);
    const PhysicalDisk& nvme = info.disks[0];
    EXPECT_EQ(nvme.name, "nvme0n1");
    EXPECT_EQ(nvme.model, "Samsung SSD 980 PRO 1TB");
    EXPECT_EQ(nvme.bus, "NVMe");
    EXPECT_EQ(nvme.media, DiskMedia::Ssd);
    EXPECT_EQ(nvme.sizeBytes, 1953525168ULL * 512);
    EXPECT_EQ(nvme.firmware, "5B2QGXA7");
    EXPECT_EQ(nvme.serial, "S5GXNX0R123456");
    EXPECT_EQ(nvme.temperatureCelsius, 41);
    EXPECT_FALSE(nvme.health.has_value());
    EXPECT_FALSE(nvme.healthUnavailableReason.empty()); // SMART is a muted dash
    const PhysicalDisk& sata = info.disks[1];
    EXPECT_EQ(sata.name, "sda");
    EXPECT_EQ(sata.bus, "SATA");
    EXPECT_EQ(sata.media, DiskMedia::Hdd);
    EXPECT_EQ(sata.firmware, "0A80");
    EXPECT_EQ(sata.serial, "WD-WCC4M1234567"); // from udev
    EXPECT_EQ(sata.temperatureCelsius, 35);    // drivetemp's hwmon/hwmonN layout

    EXPECT_TRUE(info.volumesRead);
    ASSERT_EQ(info.volumes.size(), 6U);
    EXPECT_EQ(info.volumes[0].mountPoint, "/");
    EXPECT_EQ(info.volumes[0].label, "root");
    EXPECT_EQ(info.volumes[0].device, "/dev/nvme0n1p2");
    EXPECT_TRUE(info.volumes[0].sizeRead);
    EXPECT_EQ(info.volumes[0].freeBytes, 25ULL * 1024 * 1024 * 1024);
    EXPECT_FALSE(info.volumes[2].sizeRead); // /boot/efi's statvfs failed
    EXPECT_TRUE(info.volumes[4].network);
    EXPECT_FALSE(info.volumes[4].sizeRead);
    // The NFS share is never sized: statvfs() on it can hang.
    EXPECT_EQ(std::ranges::count(sizedMounts(), std::string("/mnt/nas")), 0);
    EXPECT_EQ(sizedMounts().size(), 5U);
}

TEST(LinuxStorageTest, NothingReadableLeavesEverythingUnknown)
{
    const FixtureRoot root;
    StorageInfo info;
    LinuxStorage::readStorageFacts(root.path(), info, nullptr);
    EXPECT_TRUE(info.available);
    EXPECT_FALSE(info.disksRead);
    EXPECT_FALSE(info.volumesRead);
    EXPECT_TRUE(info.disks.empty());
    EXPECT_TRUE(info.volumes.empty());
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

} // namespace
} // namespace Platform::LinuxOsInfo
