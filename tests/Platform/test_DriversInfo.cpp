/// @file test_DriversInfo.cpp
/// @brief The Drivers readers (#1521): the /proc/modules parser and the Linux reader under a fixture root
/// (Platform/Linux/LinuxKernelModules.h), and the Windows reader through a fake function table
/// (Platform/Windows/WindowsDrivers.h). Standard library only, so it runs everywhere.

#include "Platform/IServiceProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxKernelModules.h"
#include "Platform/Windows/WindowsDrivers.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform
{
namespace
{

// Real-looking /proc/modules lines: an out-of-tree, proprietary module with taints, a module others use,
// one that can't be unloaded, one still loading, and an unprivileged reader's zeroed addresses.
constexpr std::string_view PROC_MODULES = "nvidia_uvm 4956160 0 - Live 0x0000000000000000 (POE)\n"
                                          "nvidia_drm 126976 4 - Live 0x0000000000000000 (POE)\n"
                                          "nvidia 60620800 49 nvidia_uvm,nvidia_modeset, Live 0x0000000000000000 (POE)\n"
                                          "kvm_intel 487424 0 - Live 0x0000000000000000\n"
                                          "kvm 1404928 1 kvm_intel, Live 0x0000000000000000\n"
                                          "vfio 69632 2 vfio_pci_core,vfio_iommu_type1,[permanent], Live 0x0000000000000000\n"
                                          "zfs 6418432 3 - Loading 0xffffffffc1a00000 (POE+)\n"
                                          "\n"
                                          "broken line\n"
                                          "nosize abc 0 - Live 0x0\n";

TEST(LinuxKernelModulesTest, ParsesProcModules)
{
    const std::vector<KernelDriver> modules = LinuxKernelModules::parseProcModules(PROC_MODULES);
    ASSERT_EQ(modules.size(), 7U); // the blank, short and sizeless lines are skipped
    EXPECT_EQ(modules[0].name, "nvidia_uvm");
    EXPECT_EQ(modules[0].sizeBytes, 4956160U);
    EXPECT_EQ(modules[0].useCount, std::optional<std::uint32_t>(0));
    EXPECT_TRUE(modules[0].usedBy.empty());
    EXPECT_EQ(modules[0].moduleState, "Live");
    EXPECT_EQ(modules[0].taints, "POE");

    EXPECT_EQ(modules[2].useCount, std::optional<std::uint32_t>(49));
    EXPECT_EQ(modules[2].usedBy, (std::vector<std::string>{"nvidia_uvm", "nvidia_modeset"}));
    EXPECT_TRUE(modules[3].taints.empty());

    EXPECT_TRUE(modules[5].permanent);
    EXPECT_EQ(modules[5].usedBy, (std::vector<std::string>{"vfio_pci_core", "vfio_iommu_type1"}));
    EXPECT_FALSE(modules[4].permanent);

    EXPECT_EQ(modules[6].moduleState, "Loading");
    EXPECT_EQ(modules[6].taints, "POE"); // the loading "+" is dropped
}

TEST(LinuxKernelModulesTest, KernelWithoutModuleUnloadingAndCrLf)
{
    // Without CONFIG_MODULE_UNLOAD the refcount and deps are "-".
    const std::vector<KernelDriver> modules =
        LinuxKernelModules::parseProcModules("e1000e 344064 - - Live 0x0\r\nfoo 1 2 bar, Unloading 0x0");
    ASSERT_EQ(modules.size(), 2U);
    EXPECT_FALSE(modules[0].useCount.has_value());
    EXPECT_TRUE(modules[0].usedBy.empty());
    EXPECT_EQ(modules[0].moduleState, "Live");
    EXPECT_EQ(modules[1].usedBy, (std::vector<std::string>{"bar"}));
    EXPECT_EQ(modules[1].moduleState, "Unloading");
}

/// A fresh fixture root under the temp directory, removed (best effort) at the end.
class KernelModulesFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        static std::atomic<unsigned> counter{0};
        m_Root = std::filesystem::temp_directory_path() /
                 std::format("ts_modules_{}_{}", counter.fetch_add(1), std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(m_Root);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    void write(std::string_view relative, std::string_view text) const
    {
        const auto file = m_Root / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out << text;
    }

    std::filesystem::path m_Root;
};

TEST_F(KernelModulesFixture, ReadsModulesAndVersions)
{
    write("proc/modules", PROC_MODULES);
    write("sys/module/nvidia/version", "550.54.14\n");
    write("sys/module/kvm/initstate", "live\n"); // no version file: none shown
    DriversInfo info;
    LinuxKernelModules::readKernelModules(m_Root, info);
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Linux);
    EXPECT_TRUE(info.listed);
    ASSERT_EQ(info.drivers.size(), 7U);
    EXPECT_EQ(info.drivers[2].version, "550.54.14");
    EXPECT_TRUE(info.drivers[4].version.empty());
}

TEST_F(KernelModulesFixture, NoProcModulesIsNotListed)
{
    DriversInfo info;
    LinuxKernelModules::readKernelModules(m_Root, info);
    EXPECT_TRUE(info.available);
    EXPECT_FALSE(info.listed);
    EXPECT_TRUE(info.drivers.empty());
}

TEST(WindowsDriversTest, ResolvesImagePaths)
{
    using WindowsDrivers::driverImagePath;
    constexpr std::string_view WINDOWS = "C:\\Windows";
    EXPECT_EQ(driverImagePath("\\SystemRoot\\System32\\drivers\\ACPI.sys", "ACPI", WINDOWS), "C:\\Windows\\System32\\drivers\\ACPI.sys");
    EXPECT_EQ(driverImagePath("\\systemroot\\system32\\drivers\\x.sys", "x", "C:\\Windows\\"), "C:\\Windows\\system32\\drivers\\x.sys");
    EXPECT_EQ(driverImagePath("System32\\drivers\\Wof.sys", "Wof", WINDOWS), "C:\\Windows\\System32\\drivers\\Wof.sys");
    EXPECT_EQ(driverImagePath("\\??\\C:\\Program Files\\Vendor\\drv.sys", "drv", WINDOWS), "C:\\Program Files\\Vendor\\drv.sys");
    EXPECT_EQ(driverImagePath("\"C:\\Program Files\\Vendor\\drv.sys\"", "drv", WINDOWS), "C:\\Program Files\\Vendor\\drv.sys");
    EXPECT_EQ(driverImagePath("%SystemRoot%\\System32\\drivers\\a.sys", "a", WINDOWS), "C:\\Windows\\System32\\drivers\\a.sys");
    EXPECT_EQ(driverImagePath("", "Beep", WINDOWS), "C:\\Windows\\System32\\drivers\\Beep.sys"); // the kernel's default
    EXPECT_EQ(driverImagePath("", "Beep", ""), "");
    EXPECT_EQ(WindowsDrivers::formatFileVersion(0x000A0000, 0x65F40001), "10.0.26100.1");
    EXPECT_EQ(WindowsDrivers::formatFileVersion(0, 0), "");
}

/// The fake table's answers.
std::optional<std::vector<WindowsDrivers::DriverServiceRecord>> g_Services; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::vector<std::string> g_VersionReads;                                    // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

[[nodiscard]] WindowsDrivers::Functions fakeFunctions()
{
    return {
        .listDriverServices = [] { return g_Services; },
        .windowsDirectory = [] { return std::string("C:\\Windows"); },
        .readFileVersion =
            [](const std::string& path)
        {
            g_VersionReads.push_back(path);
            return path.ends_with("ACPI.sys")
                     ? WindowsDrivers::FileVersionRecord{.version = "10.0.26100.1", .company = "Microsoft Corporation"}
                     : WindowsDrivers::FileVersionRecord{};
        },
    };
}

TEST(WindowsDriversTest, ReadsRunningDriverServicesThroughTheTable)
{
    g_VersionReads.clear();
    g_Services = std::vector<WindowsDrivers::DriverServiceRecord>{
        {
            .name = "ACPI",
            .displayName = "Microsoft ACPI Driver",
            .serviceType = 1,
            .currentState = 4,
            .startType = ServiceStartType::Boot,
            .binaryPath = R"(\SystemRoot\System32\drivers\ACPI.sys)",
        },
        {
            .name = "Ntfs",
            .displayName = "Ntfs",
            .serviceType = 2,
            .currentState = 3,
            .startType = ServiceStartType::Unknown,
            .binaryPath = "",
        },
    };
    DriversInfo info;
    WindowsDrivers::readDrivers(info, fakeFunctions());
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_TRUE(info.listed);
    ASSERT_EQ(info.drivers.size(), 2U);
    const KernelDriver& acpi = info.drivers[0];
    EXPECT_EQ(acpi.path, "C:\\Windows\\System32\\drivers\\ACPI.sys");
    EXPECT_EQ(acpi.version, "10.0.26100.1");
    EXPECT_EQ(acpi.company, "Microsoft Corporation");
    EXPECT_EQ(acpi.state, ServiceState::Running);
    EXPECT_EQ(acpi.startType, ServiceStartType::Boot);
    EXPECT_FALSE(acpi.fileSystem);
    const KernelDriver& ntfs = info.drivers[1];
    EXPECT_TRUE(ntfs.fileSystem);
    EXPECT_EQ(ntfs.state, ServiceState::StopPending);
    EXPECT_EQ(ntfs.path, "C:\\Windows\\System32\\drivers\\Ntfs.sys");
    EXPECT_TRUE(ntfs.version.empty());
    EXPECT_EQ(g_VersionReads.size(), 2U);
}

TEST(WindowsDriversTest, ScmFailureIsNotListed)
{
    g_Services = std::nullopt;
    DriversInfo info;
    WindowsDrivers::readDrivers(info, fakeFunctions());
    EXPECT_TRUE(info.available);
    EXPECT_FALSE(info.listed);
    EXPECT_TRUE(info.drivers.empty());
}

} // namespace
} // namespace Platform
