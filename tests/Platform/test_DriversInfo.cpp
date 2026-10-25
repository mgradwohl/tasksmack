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
#include <map>
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
/// Each image's file stamp (absent: unreadable), embedded and catalog WinVerifyTrust results (absent: signed
/// embedded), and how often each check ran.
struct FakeSignatures
{
    std::map<std::string, WindowsDrivers::FileStamp> stamps;
    std::map<std::string, std::int32_t> embedded;
    std::map<std::string, std::optional<std::int32_t>> catalog;
    std::map<std::string, int> embeddedChecks;
    std::map<std::string, int> catalogChecks;
    std::map<std::string, std::string> signers; ///< A trusted signature's signer (absent: "Microsoft Windows")
};

[[nodiscard]] FakeSignatures& fakes()
{
    static FakeSignatures signatures;
    return signatures;
}

[[nodiscard]] std::string signerOf(const std::string& path)
{
    const auto found = fakes().signers.find(path);
    return found != fakes().signers.end() ? found->second : std::string("Microsoft Windows");
}

constexpr std::int32_t ACCESS_DENIED = static_cast<std::int32_t>(0x80070005U); // HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)

/// Every image readable and signed, until a test says otherwise.
void resetSignatures()
{
    fakes() = FakeSignatures{};
}

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
        .errorMessage =
            [](std::uint32_t code)
        {
            switch (code)
            {
            case 31:
                return std::string("A device attached to the system is not functioning.\r\n");
            case 0x800B0101U:
                return std::string("A required certificate is not within its validity period.\r\n");
            case 0x80070005U:
                return std::string("Access is denied.\r\n");
            default:
                return std::string{};
            }
        },
        .fileStamp = [](const std::string& path) -> std::optional<WindowsDrivers::FileStamp>
        {
            if (path.ends_with("Missing.sys"))
            {
                return std::nullopt;
            }
            const auto found = fakes().stamps.find(path);
            return found != fakes().stamps.end() ? found->second : WindowsDrivers::FileStamp{.lastWriteTime = 1, .size = 1};
        },
        .verifyEmbeddedSignature =
            [](const std::string& path)
        {
            ++fakes().embeddedChecks[path];
            const auto found = fakes().embedded.find(path);
            const std::int32_t result = found != fakes().embedded.end() ? found->second : 0;
            return WindowsDrivers::TrustCheck{.result = result, .signer = result == 0 ? signerOf(path) : std::string{}};
        },
        .verifyCatalogSignature = [](const std::string& path) -> std::optional<WindowsDrivers::TrustCheck>
        {
            ++fakes().catalogChecks[path];
            const auto found = fakes().catalog.find(path);
            if (found == fakes().catalog.end() || !found->second.has_value())
            {
                return std::nullopt;
            }
            const std::int32_t result = *found->second;
            return WindowsDrivers::TrustCheck{.result = result, .signer = result == 0 ? signerOf(path) : std::string{}};
        },
    };
}

/// A driver service record for the tests below.
[[nodiscard]] WindowsDrivers::DriverServiceRecord
service(std::string name, std::uint32_t state, ServiceStartType startType, std::uint32_t exitCode = 0, std::uint32_t specificExitCode = 0)
{
    return {
        .name = name,
        .displayName = name,
        .serviceType = 1,
        .currentState = state,
        .startType = startType,
        .binaryPath = std::format(R"(\SystemRoot\System32\drivers\{}.sys)", name),
        .win32ExitCode = exitCode,
        .serviceSpecificExitCode = specificExitCode,
    };
}

[[nodiscard]] std::string imagePath(std::string_view name)
{
    return std::format(R"(C:\Windows\System32\drivers\{}.sys)", name);
}

constexpr std::uint32_t STOPPED = 1;
constexpr std::uint32_t RUNNING = 4;

TEST(WindowsDriversTest, ReadsRunningDriverServicesThroughTheTable)
{
    g_VersionReads.clear();
    resetSignatures();
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
    EXPECT_EQ(acpi.signature, DriverSignature::Embedded);
    EXPECT_TRUE(acpi.startError.empty());
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

TEST(WindowsDriversTest, ListsDriversThatFailedToStart)
{
    resetSignatures();
    fakes().signers[imagePath("hwbad")] = "Contoso Ltd."; // a third-party image: its general failure counts
    g_Services = std::vector<WindowsDrivers::DriverServiceRecord>{
        service("ACPI", RUNNING, ServiceStartType::Boot),
        service("hwbad", STOPPED, ServiceStartType::Boot, 31),
        service("svcerr", STOPPED, ServiceStartType::Automatic, WindowsDrivers::SERVICE_SPECIFIC_EXIT, 5),
        service("ntstat", STOPPED, ServiceStartType::System, 0xC0000034U), // an NTSTATUS with no message text
        // Not failures: a demand-start driver (most are for absent hardware), one that never started or
        // declined as not supported, one stopped cleanly, and a disabled one.
        service("usbcam", STOPPED, ServiceStartType::Manual, 31),
        service("WdBoot", STOPPED, ServiceStartType::Boot, WindowsDrivers::NEVER_STARTED_EXIT),
        service("privacy", STOPPED, ServiceStartType::Automatic, WindowsDrivers::NOT_SUPPORTED_EXIT),
        service("clean", STOPPED, ServiceStartType::System, 0),
        service("off", STOPPED, ServiceStartType::Disabled, 31),
    };
    DriversInfo info;
    WindowsDrivers::readDrivers(info, fakeFunctions());
    ASSERT_EQ(info.drivers.size(), 4U);
    EXPECT_TRUE(info.drivers[0].startError.empty());
    EXPECT_EQ(info.drivers[1].name, "hwbad");
    EXPECT_EQ(info.drivers[1].state, ServiceState::Stopped);
    EXPECT_EQ(info.drivers[1].startError, "A device attached to the system is not functioning"); // no period or line break
    EXPECT_EQ(info.drivers[2].startError, "service-specific error 5");
    EXPECT_EQ(info.drivers[3].startError, "error 0xC0000034");

    EXPECT_TRUE(WindowsDrivers::failedToStart(service("x", STOPPED, ServiceStartType::AutomaticDelayed, 2)));
    EXPECT_FALSE(WindowsDrivers::failedToStart(service("x", RUNNING, ServiceStartType::Boot, 31)));
    EXPECT_FALSE(WindowsDrivers::failedToStart(service("x", STOPPED, ServiceStartType::Unknown, 31)));
}

TEST(WindowsDriversTest, LeavesOutStartErrorsAHealthyMachineShows)
{
    // The maintainer's rule (#1661): a healthy machine shows no problem drivers. A Microsoft-signed image's
    // ERROR_GEN_FAILURE (hwpolicy) and a hypervisor-facility NTSTATUS (l1vhlwf without Hyper-V) aren't
    // failures; the same 31 from any other image, or another NTSTATUS, still is.
    resetSignatures();
    namespace Trust = WindowsDrivers::TrustResult;
    fakes().embedded = {
        {imagePath("hwpolicy"), Trust::NO_SIGNATURE},
        {imagePath("vendor"), Trust::NO_SIGNATURE},
        {imagePath("whql"), Trust::NO_SIGNATURE},
        {imagePath("homebrew"), Trust::NO_SIGNATURE},
    };
    fakes().catalog = {{imagePath("hwpolicy"), 0}, {imagePath("whql"), 0}}; // catalog-signed
    fakes().signers = {
        {imagePath("msembedded"), "Microsoft Corporation"},
        {imagePath("vendor"), "Contoso Ltd."},
        {imagePath("whql"), "Microsoft Windows Hardware Compatibility Publisher"}, // signs third-party drivers
    };
    g_Services = std::vector<WindowsDrivers::DriverServiceRecord>{
        service("hwpolicy", STOPPED, ServiceStartType::Boot, WindowsDrivers::GEN_FAILURE_EXIT),   // Microsoft Windows, catalog
        service("msembedded", STOPPED, ServiceStartType::Boot, WindowsDrivers::GEN_FAILURE_EXIT), // Microsoft Corporation, embedded
        service("l1vhlwf", STOPPED, ServiceStartType::Automatic, 0xC035001EU),
        service("hvother", STOPPED, ServiceStartType::System, 0xC0351000U),
        service("vendor", STOPPED, ServiceStartType::Boot, WindowsDrivers::GEN_FAILURE_EXIT),
        service("whql", STOPPED, ServiceStartType::System, WindowsDrivers::GEN_FAILURE_EXIT),
        service("homebrew", STOPPED, ServiceStartType::System, WindowsDrivers::GEN_FAILURE_EXIT), // unsigned
        service("Missing", STOPPED, ServiceStartType::System, WindowsDrivers::GEN_FAILURE_EXIT),  // signature unknown
        service("ntother", STOPPED, ServiceStartType::Boot, 0xC0000034U),
    };
    DriversInfo info;
    WindowsDrivers::readDrivers(info, fakeFunctions());
    std::vector<std::string> failed;
    for (const KernelDriver& driver : info.drivers)
    {
        EXPECT_FALSE(driver.startError.empty()) << driver.name;
        failed.push_back(driver.name);
    }
    EXPECT_EQ(failed, (std::vector<std::string>{"vendor", "whql", "homebrew", "Missing", "ntother"}));

    // The signer is cached with the signature: a cached read excuses the same drivers.
    WindowsDrivers::SignatureCache cache;
    DriversInfo first;
    WindowsDrivers::readDrivers(first, fakeFunctions(), &cache);
    DriversInfo cached;
    WindowsDrivers::readDrivers(cached, fakeFunctions(), &cache);
    EXPECT_EQ(cached.drivers.size(), info.drivers.size());
    EXPECT_EQ(fakes().embeddedChecks[imagePath("hwpolicy")], 2); // the uncached read, then the first cached one

    EXPECT_TRUE(WindowsDrivers::isHypervisorStatus(0xC035001EU));
    EXPECT_FALSE(WindowsDrivers::isHypervisorStatus(0xC0340001U));
    EXPECT_FALSE(WindowsDrivers::isStartFailureCode(0xC035001EU));
    EXPECT_TRUE(WindowsDrivers::isStartFailureCode(WindowsDrivers::GEN_FAILURE_EXIT)); // excused only by the signer
    EXPECT_TRUE(WindowsDrivers::isMicrosoftSigned(DriverSignature::Catalog, "Microsoft Windows"));
    EXPECT_FALSE(WindowsDrivers::isMicrosoftSigned(DriverSignature::Untrusted, "Microsoft Windows"));
    EXPECT_FALSE(WindowsDrivers::isMicrosoftSigned(DriverSignature::Embedded, "Microsoft Windows Hardware Compatibility Publisher"));
    EXPECT_FALSE(WindowsDrivers::isBenignInboxFailure(2, DriverSignature::Embedded, "Microsoft Windows"));
}

TEST(WindowsDriversTest, ClassifiesWinVerifyTrustResults)
{
    using WindowsDrivers::classifySignature;
    namespace Trust = WindowsDrivers::TrustResult;
    EXPECT_EQ(classifySignature(0, std::nullopt).signature, DriverSignature::Embedded);
    EXPECT_EQ(classifySignature(Trust::NO_SIGNATURE, 0).signature, DriverSignature::Catalog);
    EXPECT_EQ(classifySignature(Trust::NO_SIGNATURE, std::nullopt).signature, DriverSignature::Unsigned);
    EXPECT_EQ(classifySignature(Trust::SUBJECT_FORM_UNKNOWN, std::nullopt).signature, DriverSignature::Unsigned);
    EXPECT_EQ(classifySignature(Trust::BAD_DIGEST, std::nullopt).signature, DriverSignature::Untrusted); // tampered, no catalog
    EXPECT_EQ(classifySignature(Trust::EXPLICIT_DISTRUST, std::nullopt).result, Trust::EXPLICIT_DISTRUST);
    EXPECT_EQ(classifySignature(Trust::BAD_DIGEST, 0).signature, DriverSignature::Catalog); // a trusted catalog wins
    const WindowsDrivers::SignatureVerdict expired = classifySignature(Trust::NO_SIGNATURE, Trust::EXPIRED);
    EXPECT_EQ(expired.signature, DriverSignature::Untrusted);
    EXPECT_EQ(expired.result, Trust::EXPIRED);
    EXPECT_EQ(classifySignature(Trust::NO_SIGNATURE, ACCESS_DENIED).signature, DriverSignature::Unknown); // catalogs unreadable
    EXPECT_EQ(classifySignature(ACCESS_DENIED, std::nullopt).signature, DriverSignature::Unknown);
}

TEST(WindowsDriversTest, ChecksSignaturesEmbeddedThenCatalog)
{
    resetSignatures();
    g_Services = std::vector<WindowsDrivers::DriverServiceRecord>{
        service("vendor", RUNNING, ServiceStartType::System),   // signed embedded
        service("inbox", RUNNING, ServiceStartType::Boot),      // catalog signed
        service("homebrew", RUNNING, ServiceStartType::Manual), // unsigned
        service("stale", RUNNING, ServiceStartType::Manual),    // catalog signature expired
        service("locked", RUNNING, ServiceStartType::Manual),   // couldn't be checked
        service("Missing", RUNNING, ServiceStartType::Manual),  // image unreadable
    };
    namespace Trust = WindowsDrivers::TrustResult;
    fakes().embedded = {
        {imagePath("inbox"), Trust::NO_SIGNATURE},
        {imagePath("homebrew"), Trust::NO_SIGNATURE},
        {imagePath("stale"), Trust::NO_SIGNATURE},
        {imagePath("locked"), ACCESS_DENIED},
    };
    fakes().catalog = {{imagePath("inbox"), 0}, {imagePath("stale"), Trust::EXPIRED}};
    DriversInfo info;
    WindowsDrivers::readDrivers(info, fakeFunctions());
    ASSERT_EQ(info.drivers.size(), 6U);
    EXPECT_EQ(info.drivers[0].signature, DriverSignature::Embedded);
    EXPECT_EQ(fakes().catalogChecks.count(imagePath("vendor")), 0U); // signed embedded: no catalog search
    EXPECT_EQ(info.drivers[1].signature, DriverSignature::Catalog);
    EXPECT_TRUE(info.drivers[1].signatureNote.empty());
    EXPECT_EQ(info.drivers[2].signature, DriverSignature::Unsigned);
    EXPECT_EQ(info.drivers[3].signature, DriverSignature::Untrusted);
    EXPECT_EQ(info.drivers[3].signatureNote, "A required certificate is not within its validity period");
    EXPECT_EQ(info.drivers[4].signature, DriverSignature::Unknown);
    EXPECT_EQ(info.drivers[4].signatureNote, "Access is denied");
    EXPECT_EQ(info.drivers[5].signature, DriverSignature::Unknown);
    EXPECT_EQ(info.drivers[5].signatureNote, "The image file couldn't be read");
    EXPECT_EQ(fakes().embeddedChecks.count(imagePath("Missing")), 0U);

    // Without the signature calls nothing is checked.
    WindowsDrivers::Functions noSignatures = fakeFunctions();
    noSignatures.verifyCatalogSignature = nullptr;
    DriversInfo unchecked;
    WindowsDrivers::readDrivers(unchecked, noSignatures);
    EXPECT_EQ(unchecked.drivers[0].signature, DriverSignature::NotChecked);
}

TEST(WindowsDriversTest, CachesSignaturesByPathAndFileTime)
{
    resetSignatures();
    g_Services = std::vector<WindowsDrivers::DriverServiceRecord>{
        service("inbox", RUNNING, ServiceStartType::Boot),
        service("homebrew", RUNNING, ServiceStartType::Manual),
        service("locked", RUNNING, ServiceStartType::Manual),
    };
    namespace Trust = WindowsDrivers::TrustResult;
    fakes().embedded = {
        {imagePath("inbox"), Trust::NO_SIGNATURE},
        {imagePath("homebrew"), Trust::NO_SIGNATURE},
        {imagePath("locked"), ACCESS_DENIED},
    };
    fakes().catalog = {{imagePath("inbox"), 0}};
    WindowsDrivers::SignatureCache cache;
    DriversInfo first;
    WindowsDrivers::readDrivers(first, fakeFunctions(), &cache);
    EXPECT_EQ(cache.size(), 2U); // the failed check isn't kept
    DriversInfo second;
    WindowsDrivers::readDrivers(second, fakeFunctions(), &cache);
    EXPECT_EQ(fakes().embeddedChecks[imagePath("inbox")], 1);
    EXPECT_EQ(fakes().catalogChecks[imagePath("inbox")], 1);
    EXPECT_EQ(fakes().embeddedChecks[imagePath("homebrew")], 1);
    EXPECT_EQ(fakes().embeddedChecks[imagePath("locked")], 2); // tried again
    EXPECT_EQ(second.drivers[0].signature, DriverSignature::Catalog);
    EXPECT_EQ(second.drivers[1].signature, DriverSignature::Unsigned);

    // A rewritten image (new file time) is checked again, and its new answer replaces the old one.
    fakes().stamps[imagePath("homebrew")] = {.lastWriteTime = 2, .size = 1};
    fakes().embedded[imagePath("homebrew")] = 0;
    DriversInfo third;
    WindowsDrivers::readDrivers(third, fakeFunctions(), &cache);
    EXPECT_EQ(fakes().embeddedChecks[imagePath("homebrew")], 2);
    EXPECT_EQ(third.drivers[1].signature, DriverSignature::Embedded);
    EXPECT_EQ(cache.size(), 2U);

    // Without a cache every read checks again.
    DriversInfo uncached;
    WindowsDrivers::readDrivers(uncached, fakeFunctions());
    EXPECT_EQ(fakes().embeddedChecks[imagePath("inbox")], 2);
}

} // namespace
} // namespace Platform
