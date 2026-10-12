/// @file test_WindowsSystemInfoLiveReaders.cpp
/// @brief The real function tables behind the Security (#1514), Sensors (#1522), Recent crashes (#1524) and
/// Boot performance (#1525) sections (#1395). test_WindowsSystemInfoReaders.cpp and test_CrashesInfo.cpp drive
/// the parsers through fakes; these call the Win32 readers themselves, given inputs that must fail (a malformed
/// query, an information class that doesn't exist) and inputs any Windows machine can answer, and check each
/// answer is one the parsers expect: an error code they handle, or a well-formed result.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WindowsBootPerformance.h"
#include "Platform/Windows/WindowsCrashEvents.h"
#include "Platform/Windows/WindowsPlatformSecurity.h"
#include "Platform/Windows/WindowsSystemInfoProbe.h"
#include "Platform/Windows/WindowsThermalZones.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

// An XPath that no event can match: one event has one id.
constexpr const char* NO_EVENT_XPATH = "*[System[EventID=1 and EventID=2]]";
// Not an XPath at all.
constexpr const char* MALFORMED_XPATH = "*[System[";

// ========== Event Log readers ==========

TEST(WindowsCrashEventsLiveTest, MalformedQueryFailsWithItsError)
{
    const WindowsCrashEvents::Functions fns = WindowsCrashEvents::systemFunctions();
    std::uint32_t error = 0;
    void* query = fns.openQuery(MALFORMED_XPATH, error);
    EXPECT_EQ(query, nullptr);
    EXPECT_NE(error, 0U);
    if (query != nullptr)
    {
        fns.closeQuery(query);
    }
}

TEST(WindowsCrashEventsLiveTest, QueryThatMatchesNothingEndsAtOnce)
{
    const WindowsCrashEvents::Functions fns = WindowsCrashEvents::systemFunctions();
    std::uint32_t error = 0;
    void* query = fns.openQuery(NO_EVENT_XPATH, error);
    ASSERT_NE(query, nullptr) << "the Application log is always there and readable; error " << error;
    std::string xml = "untouched";
    EXPECT_FALSE(fns.nextEventXml(query, xml));
    EXPECT_EQ(xml, "untouched") << "nothing is rendered when there is no event";
    fns.closeQuery(query);
}

TEST(WindowsCrashEventsLiveTest, NewestApplicationEventRendersAsXml)
{
    const WindowsCrashEvents::Functions fns = WindowsCrashEvents::systemFunctions();
    std::uint32_t error = 0;
    void* query = fns.openQuery("*", error);
    ASSERT_NE(query, nullptr) << "error " << error;
    std::string xml;
    if (fns.nextEventXml(query, xml))
    {
        EXPECT_TRUE(xml.starts_with("<Event")) << xml.substr(0, 80);
        EXPECT_NE(xml.find("<System>"), std::string::npos);
    }
    fns.closeQuery(query);
}

TEST(WindowsCrashEventsLiveTest, ReadsTheRealLog)
{
    CrashesInfo info;
    WindowsCrashEvents::readCrashes(info, WindowsCrashEvents::systemFunctions());
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_TRUE(info.listed) << info.unavailableReason;
    EXPECT_LE(info.events.size(), CRASH_LIST_MAX);
}

// The Diagnostics-Performance log may be missing (Server Core) or need more permission than the test has; a
// read that fails says which, with a code readBootPerformance() turns into its reason.
[[nodiscard]] bool isExpectedBootLogError(std::uint32_t error)
{
    return error == ERROR_NO_MORE_ITEMS || error == WindowsBootPerformance::ERROR_EVT_CHANNEL_NOT_FOUND_CODE ||
           error == ERROR_ACCESS_DENIED;
}

TEST(WindowsBootPerformanceLiveTest, MalformedQueryFailsWithItsError)
{
    const WindowsBootPerformance::Functions fns = WindowsBootPerformance::systemFunctions();
    std::string xml = "untouched";
    std::uint32_t error = 0;
    EXPECT_FALSE(fns.newestEventXml(MALFORMED_XPATH, xml, error));
    EXPECT_NE(error, 0U);
    EXPECT_EQ(xml, "untouched");
}

TEST(WindowsBootPerformanceLiveTest, QueryThatMatchesNothingIsNoMoreItems)
{
    const WindowsBootPerformance::Functions fns = WindowsBootPerformance::systemFunctions();
    std::string xml = "untouched";
    std::uint32_t error = 0;
    EXPECT_FALSE(fns.newestEventXml(NO_EVENT_XPATH, xml, error));
    EXPECT_TRUE(isExpectedBootLogError(error)) << "error " << error;
    EXPECT_EQ(xml, "untouched");
}

TEST(WindowsBootPerformanceLiveTest, NewestEventRendersAsXmlOrSaysWhyNot)
{
    const WindowsBootPerformance::Functions fns = WindowsBootPerformance::systemFunctions();
    std::string xml;
    std::uint32_t error = 0;
    if (fns.newestEventXml("*", xml, error))
    {
        EXPECT_EQ(error, 0U);
        EXPECT_TRUE(xml.starts_with("<Event")) << xml.substr(0, 80);
    }
    else
    {
        EXPECT_TRUE(isExpectedBootLogError(error)) << "error " << error;
    }
}

// ========== Thermal zones ==========

TEST(WindowsThermalZonesLiveTest, ZonesAreNamedOrAbsentWithTheStatus)
{
    const WindowsThermalZones::Functions fns = WindowsThermalZones::systemFunctions();
    std::vector<WindowsThermalZones::ZoneSample> zones;
    const std::uint32_t status = fns.readZones(zones);
    if (status != 0)
    {
        EXPECT_TRUE(zones.empty()) << "a failed read lists nothing; status " << status;
    }
    std::size_t i = 0;
    while (i < zones.size())
    {
        EXPECT_FALSE(zones[i].instance.empty());
        EXPECT_GE(zones[i].kelvin, 0.0);
        ++i;
    }
}

// ========== Platform security ==========

TEST(WindowsPlatformSecurityLiveTest, CpuVendorIsTwelveCharactersOnX86)
{
    const std::string vendor = WindowsPlatformSecurity::systemFunctions().cpuVendor();
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    EXPECT_EQ(vendor.size(), 12U) << vendor;
#else
    EXPECT_TRUE(vendor.empty()) << vendor;
#endif
}

TEST(WindowsPlatformSecurityLiveTest, UnknownInformationClassFails)
{
    std::array<std::byte, 64> buffer{};
    // No such class: NtQuerySystemInformation answers STATUS_INVALID_INFO_CLASS (or another error status).
    constexpr std::uint32_t NO_SUCH_CLASS = 0xFFFF;
    const std::int32_t status = WindowsPlatformSecurity::systemFunctions().querySystemInformation(NO_SUCH_CLASS, buffer);
    EXPECT_LT(status, 0) << "an NTSTATUS error has its top bit set";
}

TEST(WindowsPlatformSecurityLiveTest, SecureBootAndFirmwareAgree)
{
    const WindowsPlatformSecurity::Functions fns = WindowsPlatformSecurity::systemFunctions();
    const std::optional<std::uint32_t> secureBoot = fns.secureBootEnabledValue();
    const std::optional<bool> uefi = fns.bootedWithUefi();
    if (secureBoot.has_value())
    {
        EXPECT_LE(*secureBoot, 1U);
    }
    if (secureBoot == 1U && uefi.has_value())
    {
        EXPECT_TRUE(*uefi) << "Secure Boot is UEFI's";
    }
}

TEST(WindowsPlatformSecurityLiveTest, TpmVersionOnlyWithSuccess)
{
    std::uint32_t version = 0xDEAD;
    const std::optional<std::uint32_t> result = WindowsPlatformSecurity::systemFunctions().tpmDeviceInfo(version);
    if (!result.has_value())
    {
        GTEST_SKIP() << "tbs.dll or Tbsi_GetDeviceInfo isn't available";
    }
    if (*result == WindowsPlatformSecurity::TBS_SUCCESS_CODE)
    {
        EXPECT_TRUE(version == WindowsPlatformSecurity::TPM_VERSION_12_CODE || version == WindowsPlatformSecurity::TPM_VERSION_20_CODE)
            << version;
    }
    else
    {
        EXPECT_EQ(version, 0U) << "a failed query reports no version";
    }
}

// ========== The probe's section readers ==========

TEST(WindowsSystemInfoProbeLiveTest, SectionsReadThroughTheRealTables)
{
    WindowsSystemInfoProbe probe;

    const PlatformSecurityInfo security = probe.readPlatformSecurity();
    EXPECT_TRUE(security.available);
    EXPECT_EQ(security.family, OsFamily::Windows);
    EXPECT_LE(security.tpmVersionMajor, 2U);

    const SensorsInfo sensors = probe.readSensors();
    EXPECT_TRUE(sensors.available);
    EXPECT_EQ(sensors.family, OsFamily::Windows);

    const CrashesInfo crashes = probe.readCrashes();
    EXPECT_TRUE(crashes.available);
    EXPECT_EQ(crashes.family, OsFamily::Windows);

    const BootPerformanceInfo boot = probe.readBootPerformance();
    EXPECT_TRUE(boot.available);
    EXPECT_EQ(boot.family, OsFamily::Windows);
    EXPECT_TRUE(boot.timingsRead || !boot.unavailableReason.empty()) << "a boot that can't be read says why";
}

} // namespace
} // namespace Platform
