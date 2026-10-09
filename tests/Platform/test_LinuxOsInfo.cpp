/// @file test_LinuxOsInfo.cpp
/// @brief Platform::LinuxOsInfo (#1512): os-release parsing, the zone from the /etc/localtime link,
/// the locale choice, btime, and the file facts and container/VM hints read under a fixture root.
/// LinuxOsInfo.h uses only the standard library, so these build and run on every platform.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxOsInfo.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string_view>
#include <system_error>

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

} // namespace
} // namespace Platform::LinuxOsInfo
