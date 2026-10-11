/// @file test_WindowsSystemInfoReaders.cpp
/// @brief The Windows halves of the Security (#1514), Sensors (#1522) and Boot performance (#1525) readers,
/// through fake function tables: the boot event parser and its errors (WindowsBootPerformance.h), the
/// thermal zone conversion, PDH statuses and per-tick read (WindowsThermalZones.h), and Secure Boot, the TPM, HVCI, Kernel
/// DMA Protection and the speculation-control decoding (WindowsPlatformSecurity.h). Standard library only,
/// so it runs everywhere.

#include "Platform/ISensorProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WindowsBootPerformance.h"
#include "Platform/Windows/WindowsPlatformSecurity.h"
#include "Platform/Windows/WindowsThermalZones.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform
{
namespace
{

// ========== Boot performance (#1525) ==========

// An event 100 as EvtRender(EvtRenderEventXml) renders it on Windows 11 (most data items left out).
constexpr std::string_view BOOT_EVENT_XML =
    "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System>"
    "<Provider Name='Microsoft-Windows-Diagnostics-Performance' Guid='{cfc18ec0-96b1-4eba-961b-622caee05b0a}'/>"
    "<EventID>100</EventID><Version>2</Version><Level>2</Level><Task>4002</Task><Opcode>34</Opcode>"
    "<Keywords>0x8000000000010000</Keywords><TimeCreated SystemTime='2026-10-09T07:02:41.5512345Z'/>"
    "<EventRecordID>812</EventRecordID><Channel>Microsoft-Windows-Diagnostics-Performance/Operational</Channel>"
    "<Computer>DESKTOP-TEST</Computer><Security UserID='S-1-5-19'/></System><EventData>"
    "<Data Name='BootTsVersion'>2</Data><Data Name='BootStartTime'>2026-10-09T07:00:12.0000000Z</Data>"
    "<Data Name='BootEndTime'>2026-10-09T07:01:41.5000000Z</Data><Data Name='BootTime'>34567</Data>"
    "<Data Name='MainPathBootTime'>14200</Data><Data Name='BootKernelInitTime'>48</Data>"
    "<Data Name='BootPostBootTime'>20367</Data><Data Name='BootIsDegradation'>false</Data></EventData></Event>";

std::string g_BootXml;         // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::uint32_t g_BootError = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::string g_BootXPath;       // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

[[nodiscard]] WindowsBootPerformance::Functions fakeBootFunctions(std::string xml, std::uint32_t error)
{
    g_BootXml = std::move(xml);
    g_BootError = error;
    g_BootXPath.clear();
    return {
        .newestEventXml = [](const std::string& xpath, std::string& out, std::uint32_t& outError) -> bool
        {
            g_BootXPath = xpath;
            if (g_BootError != 0)
            {
                outError = g_BootError;
                return false;
            }
            out = g_BootXml;
            return true;
        },
    };
}

TEST(WindowsBootPerformanceTest, ParsesTheBootEvent)
{
    const auto parsed = WindowsBootPerformance::parseBootEventXml(BOOT_EVENT_XML);
    ASSERT_TRUE(parsed.has_value());
    const WindowsBootPerformance::BootEvent event = parsed.value_or(WindowsBootPerformance::BootEvent{});
    EXPECT_EQ(event.bootMs, 34567U);
    EXPECT_EQ(event.mainPathMs, 14200U);
    EXPECT_EQ(event.postBootMs, 20367U);
    EXPECT_EQ(event.unixSeconds, 1'791'529'361U); // 2026-10-09T07:02:41Z
}

TEST(WindowsBootPerformanceTest, RejectsOtherEventsAndMissingTimes)
{
    std::string other(BOOT_EVENT_XML);
    other.replace(other.find("<EventID>100<"), std::string_view("<EventID>100<").size(), "<EventID>101<");
    EXPECT_FALSE(WindowsBootPerformance::parseBootEventXml(other).has_value());
    EXPECT_FALSE(WindowsBootPerformance::parseBootEventXml("not xml").has_value());
    EXPECT_FALSE(WindowsBootPerformance::parseBootEventXml("<Event><System><EventID>100</EventID></System></Event>").has_value());

    // Without BootTime, the total is the main path plus post-boot.
    const auto summed = WindowsBootPerformance::parseBootEventXml("<Event><System><EventID>100</EventID></System><EventData>"
                                                                  "<Data Name='MainPathBootTime'>1000</Data>"
                                                                  "<Data Name='BootPostBootTime'>250</Data></EventData></Event>");
    ASSERT_TRUE(summed.has_value());
    EXPECT_EQ(summed.value_or(WindowsBootPerformance::BootEvent{}).bootMs, 1250U);
    EXPECT_EQ(summed.value_or(WindowsBootPerformance::BootEvent{}).unixSeconds, 0U);

    // A value that isn't a whole number is left out.
    EXPECT_FALSE(WindowsBootPerformance::parseMilliseconds(std::string("12a")).has_value());
    EXPECT_FALSE(WindowsBootPerformance::parseMilliseconds(std::string()).has_value());
    EXPECT_FALSE(WindowsBootPerformance::parseMilliseconds(std::nullopt).has_value());
}

TEST(WindowsBootPerformanceTest, ReadsTheNewestBootInMicroseconds)
{
    BootPerformanceInfo info;
    WindowsBootPerformance::readBootPerformance(info, fakeBootFunctions(std::string(BOOT_EVENT_XML), 0));
    EXPECT_EQ(g_BootXPath, "*[System[EventID=100]]");
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_TRUE(info.timingsRead);
    EXPECT_TRUE(info.finished);
    EXPECT_EQ(info.totalUs, 34'567'000U);
    EXPECT_EQ(info.mainPathUs, 14'200'000U);
    EXPECT_EQ(info.postBootUs, 20'367'000U);
    EXPECT_EQ(info.recordedUnixSeconds, 1'791'529'361U);
    EXPECT_FALSE(info.firmwareUs.has_value()); // the systemd phases stay unmeasured
    EXPECT_FALSE(info.userspaceUs.has_value());
}

TEST(WindowsBootPerformanceTest, ErrorsSayWhy)
{
    BootPerformanceInfo denied;
    WindowsBootPerformance::readBootPerformance(denied, fakeBootFunctions({}, WindowsBootPerformance::ERROR_ACCESS_DENIED_CODE));
    EXPECT_TRUE(denied.available);
    EXPECT_FALSE(denied.timingsRead);
    EXPECT_TRUE(denied.unavailableReason.starts_with("Requires administrator")) << denied.unavailableReason;

    BootPerformanceInfo none;
    WindowsBootPerformance::readBootPerformance(none, fakeBootFunctions({}, WindowsBootPerformance::ERROR_NO_MORE_ITEMS_CODE));
    EXPECT_FALSE(none.timingsRead);
    EXPECT_EQ(none.unavailableReason, "No boot has been recorded in the Diagnostics-Performance event log yet");

    BootPerformanceInfo missing;
    WindowsBootPerformance::readBootPerformance(missing, fakeBootFunctions({}, WindowsBootPerformance::ERROR_EVT_CHANNEL_NOT_FOUND_CODE));
    EXPECT_EQ(missing.unavailableReason, "The Diagnostics-Performance event log isn't available (missing or disabled)");

    constexpr std::uint32_t OTHER_ERROR = 1717;
    BootPerformanceInfo other;
    WindowsBootPerformance::readBootPerformance(other, fakeBootFunctions({}, OTHER_ERROR));
    EXPECT_EQ(other.unavailableReason, "The Diagnostics-Performance event log couldn't be read (error 1717)");

    BootPerformanceInfo garbled;
    WindowsBootPerformance::readBootPerformance(garbled, fakeBootFunctions("<Event/>", 0));
    EXPECT_FALSE(garbled.timingsRead);
    EXPECT_EQ(garbled.unavailableReason, "The last boot's event has no boot time in it");
}

// ========== Thermal zones (#1522) ==========

std::vector<WindowsThermalZones::ZoneSample> g_Zones; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::uint32_t g_ZoneStatus = 0;                       // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

[[nodiscard]] WindowsThermalZones::Functions fakeZoneFunctions(std::vector<WindowsThermalZones::ZoneSample> zones, std::uint32_t status)
{
    g_Zones = std::move(zones);
    g_ZoneStatus = status;
    return {
        .readZones = [](std::vector<WindowsThermalZones::ZoneSample>& out) -> std::uint32_t
        {
            out = g_Zones;
            return g_ZoneStatus;
        },
    };
}

TEST(WindowsThermalZonesTest, LabelsDropTheAcpiScope)
{
    EXPECT_EQ(WindowsThermalZones::zoneLabel("\\_TZ.TZ00"), "TZ00");
    EXPECT_EQ(WindowsThermalZones::zoneLabel("_TZ.THM0"), "THM0");
    EXPECT_EQ(WindowsThermalZones::zoneLabel("\\_SB.PCI0.TZ01"), "_SB.PCI0.TZ01");
    EXPECT_EQ(WindowsThermalZones::zoneLabel(""), "Thermal zone");
}

TEST(WindowsThermalZonesTest, ZonesAreOneDeviceInCelsius)
{
    std::vector<WindowsThermalZones::ZoneSample> zones{
        {.instance = "\\_TZ.TZ01", .kelvin = 318.15},
        {.instance = "\\_TZ.TZ00", .kelvin = 300.0},
        {.instance = "\\_TZ.TZ02", .kelvin = 0.0}, // unpopulated
    };
    SensorsInfo info;
    WindowsThermalZones::readSensors(info, fakeZoneFunctions(std::move(zones), 0));
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_TRUE(info.listed);
    ASSERT_EQ(info.devices.size(), 1U);
    EXPECT_EQ(info.devices[0].name, "Thermal zones");
    ASSERT_EQ(info.devices[0].readings.size(), 2U);
    EXPECT_EQ(info.devices[0].readings[0].label, "TZ00");
    EXPECT_NEAR(info.devices[0].readings[0].value, 26.85, 1e-9);
    EXPECT_EQ(info.devices[0].readings[1].label, "TZ01");
    EXPECT_NEAR(info.devices[0].readings[1].value, 45.0, 1e-9);
    EXPECT_EQ(info.devices[0].readings[1].kind, SensorKind::Temperature);
    EXPECT_FALSE(info.devices[0].readings[1].high.has_value());
}

TEST(WindowsThermalZonesTest, NoZonesIsListedAndAFailureIsNot)
{
    constexpr std::array<std::uint32_t, 5> NO_ZONES{
        WindowsThermalZones::PDH_CSTATUS_NO_OBJECT_CODE,
        WindowsThermalZones::PDH_CSTATUS_NO_COUNTER_CODE,
        WindowsThermalZones::PDH_CSTATUS_NO_INSTANCE_CODE,
        WindowsThermalZones::PDH_NO_DATA_CODE,
        0U,
    };
    for (const std::uint32_t status : NO_ZONES)
    {
        SensorsInfo none;
        WindowsThermalZones::readSensors(none, fakeZoneFunctions({}, status));
        EXPECT_TRUE(none.available);
        EXPECT_TRUE(none.listed) << status;
        EXPECT_TRUE(none.devices.empty());
    }

    constexpr std::uint32_t PDH_ACCESS_DENIED = 0xC0000BDBU;
    SensorsInfo failed;
    WindowsThermalZones::readSensors(failed, fakeZoneFunctions({{.instance = "\\_TZ.TZ00", .kelvin = 300.0}}, PDH_ACCESS_DENIED));
    EXPECT_TRUE(failed.available);
    EXPECT_FALSE(failed.listed);
    EXPECT_TRUE(failed.devices.empty());
}

TEST(WindowsThermalZonesTest, ZoneValuesMatchByInstance)
{
    const std::vector<WindowsThermalZones::ZoneSample> zones{
        {.instance = "\\_TZ.TZ01", .kelvin = 320.15},
        {.instance = "\\_TZ.TZ00", .kelvin = 0.0}, // unpopulated this time
    };
    const std::vector<std::string> instances{"\\_TZ.TZ00", "\\_TZ.TZ01", "\\_TZ.GONE"};
    std::vector<double> values(4, -1.0);
    WindowsThermalZones::zoneValues(zones, instances, values);
    EXPECT_TRUE(std::isnan(values[0]));
    EXPECT_NEAR(values[1], 47.0, 1e-9);
    EXPECT_TRUE(std::isnan(values[2])); // missing from this collection
    EXPECT_DOUBLE_EQ(values[3], -1.0);  // beyond the zones listed: left alone
}

TEST(WindowsThermalZonesTest, SensorProbeReadsTheListedZonesPerTick)
{
    // A reader standing in for the kept-open PDH query: each call is one collection.
    struct Collections
    {
        std::vector<std::vector<WindowsThermalZones::ZoneSample>> answers;
        std::uint32_t status = 0;
        std::size_t next = 0;
    };
    const auto collections = std::make_shared<Collections>();
    collections->answers = {
        {{.instance = "\\_TZ.TZ01", .kelvin = 318.15}, {.instance = "\\_TZ.TZ00", .kelvin = 300.0}},
        {{.instance = "\\_TZ.TZ00", .kelvin = 305.0}, {.instance = "\\_TZ.TZ01", .kelvin = 330.15}},
    };
    WindowsThermalZones::ThermalZoneSensorProbe probe(
        [collections](std::vector<WindowsThermalZones::ZoneSample>& zones) -> std::uint32_t
        {
            const std::size_t index = std::min(collections->next, collections->answers.size() - 1);
            ++collections->next;
            zones = collections->answers[index];
            return collections->status;
        });

    const SensorsInfo info = probe.discover();
    ASSERT_EQ(sensorCount(info), 2U);
    EXPECT_EQ(info.devices[0].readings[0].label, "TZ00"); // sorted by label, as the page lists them
    EXPECT_EQ(info.devices[0].readings[1].label, "TZ01");

    std::vector<double> values(2, 0.0);
    probe.readValues(values);
    EXPECT_NEAR(values[0], 31.85, 1e-9); // TZ00, whatever order the collection lists it in
    EXPECT_NEAR(values[1], 57.0, 1e-9);

    constexpr std::uint32_t PDH_ACCESS_DENIED = 0xC0000BDBU;
    collections->status = PDH_ACCESS_DENIED;
    probe.readValues(values);
    EXPECT_TRUE(std::isnan(values[0])); // a failed collection is a gap for every zone
    EXPECT_TRUE(std::isnan(values[1]));
}

TEST(WindowsThermalZonesTest, SensorProbeWithNoZonesCollectsNothingPerTick)
{
    const auto calls = std::make_shared<int>(0);
    WindowsThermalZones::ThermalZoneSensorProbe probe(
        [calls](std::vector<WindowsThermalZones::ZoneSample>& /*zones*/) -> std::uint32_t
        {
            ++*calls;
            return WindowsThermalZones::PDH_CSTATUS_NO_OBJECT_CODE;
        });
    const SensorsInfo info = probe.discover();
    EXPECT_TRUE(info.listed);
    EXPECT_EQ(sensorCount(info), 0U);
    probe.readValues({});
    probe.readValues({});
    EXPECT_EQ(*calls, 1); // discover() only
}

// ========== Security (#1514) ==========

struct FakeSecurity
{
    std::optional<bool> uefi = true;
    std::optional<std::uint32_t> secureBoot = 1;
    std::optional<std::uint32_t> tbsResult = WindowsPlatformSecurity::TBS_SUCCESS_CODE;
    std::uint32_t tpmVersion = WindowsPlatformSecurity::TPM_VERSION_20_CODE;
    /// NtQuerySystemInformation's answers by class: the bytes written, or absent for STATUS_INVALID_INFO_CLASS.
    std::vector<std::pair<std::uint32_t, std::vector<std::byte>>> answers;
    std::string vendor = "GenuineIntel";
    std::uint32_t codeIntegrityLengthSeen = 0;
};

/// The fake table's state, set by fakeSecurityFunctions().
[[nodiscard]] FakeSecurity& fakeState()
{
    static FakeSecurity state;
    return state;
}

/// Answer @p infoClass with @p bytes from now on.
void answer(FakeSecurity& fake, std::uint32_t infoClass, std::vector<std::byte> bytes)
{
    std::erase_if(fake.answers, [infoClass](const auto& item) { return item.first == infoClass; });
    fake.answers.emplace_back(infoClass, std::move(bytes));
}

[[nodiscard]] std::vector<std::byte> words(std::initializer_list<std::uint32_t> values)
{
    std::vector<std::byte> out(values.size() * sizeof(std::uint32_t));
    std::size_t at = 0;
    for (const std::uint32_t value : values)
    {
        std::memcpy(out.data() + at, &value, sizeof(value));
        at += sizeof(value);
    }
    return out;
}

[[nodiscard]] WindowsPlatformSecurity::Functions fakeSecurityFunctions(FakeSecurity fake)
{
    fakeState() = std::move(fake);
    return {
        .secureBootEnabledValue = [] { return fakeState().secureBoot; },
        .bootedWithUefi = [] { return fakeState().uefi; },
        .tpmDeviceInfo = [](std::uint32_t& version) -> std::optional<std::uint32_t>
        {
            version = fakeState().tpmVersion;
            return fakeState().tbsResult;
        },
        .querySystemInformation = [](std::uint32_t infoClass, std::span<std::byte> buffer) -> std::int32_t
        {
            constexpr auto STATUS_INVALID_INFO_CLASS = static_cast<std::int32_t>(0xC0000003U);
            constexpr auto STATUS_INFO_LENGTH_MISMATCH = static_cast<std::int32_t>(0xC0000004U);
            if (infoClass == WindowsPlatformSecurity::SYSTEM_CODE_INTEGRITY_INFORMATION)
            {
                std::memcpy(&fakeState().codeIntegrityLengthSeen, buffer.data(), sizeof(std::uint32_t));
            }
            const auto& answers = fakeState().answers;
            const auto it = std::ranges::find(answers, infoClass, &std::pair<std::uint32_t, std::vector<std::byte>>::first);
            if (it == answers.end())
            {
                return STATUS_INVALID_INFO_CLASS;
            }
            if (buffer.size() != it->second.size())
            {
                return STATUS_INFO_LENGTH_MISMATCH; // these classes want their exact size
            }
            std::memcpy(buffer.data(), it->second.data(), buffer.size());
            return 0;
        },
        .cpuVendor = [] { return fakeState().vendor; },
    };
}

/// The flags this machine's Windows 11 (Intel Core Ultra 7 255H) reports.
[[nodiscard]] FakeSecurity modernIntel()
{
    FakeSecurity fake;
    answer(fake, WindowsPlatformSecurity::SYSTEM_CODE_INTEGRITY_INFORMATION, words({8, 0xF401U}));
    answer(fake, WindowsPlatformSecurity::SYSTEM_DMA_GUARD_POLICY_INFORMATION, std::vector<std::byte>{std::byte{1}});
    answer(fake, WindowsPlatformSecurity::SYSTEM_SPECULATION_CONTROL_INFORMATION, words({0x7D8F'93F9U}));
    answer(fake, WindowsPlatformSecurity::SYSTEM_KERNEL_VA_SHADOW_INFORMATION, words({0x3020U}));
    return fake;
}

[[nodiscard]] std::string statusOf(const PlatformSecurityInfo& info, std::string_view name)
{
    for (const CpuVulnerability& item : info.vulnerabilities)
    {
        if (item.name == name)
        {
            return item.status;
        }
    }
    return "(absent)";
}

TEST(WindowsPlatformSecurityTest, ReadsEveryFact)
{
    PlatformSecurityInfo info;
    WindowsPlatformSecurity::readPlatformSecurity(info, fakeSecurityFunctions(modernIntel()));
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_EQ(info.secureBoot, SecurityFeatureState::On);
    EXPECT_EQ(info.tpm, SecurityFeatureState::On);
    EXPECT_EQ(info.tpmVersionMajor, 2U);
    EXPECT_EQ(fakeState().codeIntegrityLengthSeen, 8U); // SYSTEM_CODEINTEGRITY_INFORMATION::Length was set
    EXPECT_EQ(info.memoryIntegrity, true);
    EXPECT_EQ(info.kernelDmaProtection, true);
    EXPECT_TRUE(info.vulnerabilitiesRead);
    ASSERT_EQ(info.vulnerabilities.size(), 5U);
    EXPECT_EQ(info.vulnerabilities.front().name, "l1tf"); // sorted by name
    EXPECT_EQ(statusOf(info, "spectre_v2"), "Mitigation: Enhanced IBRS");
    EXPECT_EQ(statusOf(info, "spec_store_bypass"), "Vulnerable: Speculative Store Bypass Disable is off (the Windows default)");
    EXPECT_EQ(statusOf(info, "mds"), "Not affected");
    EXPECT_EQ(statusOf(info, "meltdown"), "Not affected");
    EXPECT_EQ(statusOf(info, "l1tf"), "Not affected");
    EXPECT_TRUE(info.lsms.empty()); // the Linux facts stay unread
    EXPECT_FALSE(info.selinuxEnforcing.has_value());
}

TEST(WindowsPlatformSecurityTest, SecureBootAndTpmStates)
{
    FakeSecurity off = modernIntel();
    off.secureBoot = 0;
    off.tbsResult = WindowsPlatformSecurity::TBS_E_TPM_NOT_FOUND_CODE;
    PlatformSecurityInfo info;
    WindowsPlatformSecurity::readPlatformSecurity(info, fakeSecurityFunctions(off));
    EXPECT_EQ(info.secureBoot, SecurityFeatureState::Off);
    EXPECT_EQ(info.tpm, SecurityFeatureState::NotSupported);
    EXPECT_EQ(info.tpmVersionMajor, 0U);

    FakeSecurity legacy = modernIntel();
    legacy.uefi = false;
    legacy.secureBoot = std::nullopt; // no SecureBoot\State key on a BIOS boot
    legacy.tbsResult = std::nullopt;  // tbs.dll missing
    PlatformSecurityInfo legacyInfo;
    WindowsPlatformSecurity::readPlatformSecurity(legacyInfo, fakeSecurityFunctions(legacy));
    EXPECT_EQ(legacyInfo.secureBoot, SecurityFeatureState::NotSupported);
    EXPECT_EQ(legacyInfo.tpm, SecurityFeatureState::Unknown);

    FakeSecurity unknown = modernIntel();
    unknown.uefi = std::nullopt;
    unknown.secureBoot = std::nullopt;
    constexpr std::uint32_t TBS_E_SERVICE_NOT_RUNNING = 0x8028'4008U;
    unknown.tbsResult = TBS_E_SERVICE_NOT_RUNNING;
    unknown.tpmVersion = 1;
    PlatformSecurityInfo unknownInfo;
    WindowsPlatformSecurity::readPlatformSecurity(unknownInfo, fakeSecurityFunctions(unknown));
    EXPECT_EQ(unknownInfo.secureBoot, SecurityFeatureState::Unknown);
    EXPECT_EQ(unknownInfo.tpm, SecurityFeatureState::Unknown);

    FakeSecurity tpm12 = modernIntel();
    tpm12.tpmVersion = WindowsPlatformSecurity::TPM_VERSION_12_CODE;
    PlatformSecurityInfo tpm12Info;
    WindowsPlatformSecurity::readPlatformSecurity(tpm12Info, fakeSecurityFunctions(tpm12));
    EXPECT_EQ(tpm12Info.tpmVersionMajor, 1U);
}

TEST(WindowsPlatformSecurityTest, FailedQueriesLeaveTheirFactsUnread)
{
    FakeSecurity old = modernIntel();
    old.answers.clear(); // an old Windows without any of the classes
    PlatformSecurityInfo info;
    WindowsPlatformSecurity::readPlatformSecurity(info, fakeSecurityFunctions(old));
    EXPECT_FALSE(info.memoryIntegrity.has_value());
    EXPECT_FALSE(info.kernelDmaProtection.has_value());
    EXPECT_FALSE(info.vulnerabilitiesRead);
    EXPECT_TRUE(info.vulnerabilities.empty());
    EXPECT_EQ(info.secureBoot, SecurityFeatureState::On); // the rest is unaffected

    FakeSecurity off = modernIntel();
    answer(off, WindowsPlatformSecurity::SYSTEM_CODE_INTEGRITY_INFORMATION, words({8, 0x0001U})); // CI on, HVCI off
    answer(off, WindowsPlatformSecurity::SYSTEM_DMA_GUARD_POLICY_INFORMATION, std::vector<std::byte>{std::byte{0}});
    PlatformSecurityInfo offInfo;
    WindowsPlatformSecurity::readPlatformSecurity(offInfo, fakeSecurityFunctions(off));
    EXPECT_EQ(offInfo.memoryIntegrity, false);
    EXPECT_EQ(offInfo.kernelDmaProtection, false);
}

TEST(WindowsPlatformSecurityTest, DecodesSpectreV2)
{
    using WindowsPlatformSecurity::decodeVulnerabilities;
    const auto spectre = [](std::uint32_t flags)
    {
        const auto list = decodeVulnerabilities(flags, std::nullopt, "GenuineIntel");
        return list.empty() ? std::string{} : list.back().status; // spectre_v2 sorts last
    };
    using namespace WindowsPlatformSecurity;
    EXPECT_EQ(spectre(SPEC_BPB_ENABLED | SPEC_ENHANCED_IBRS | SPEC_RETPOLINE_ENABLED), "Mitigation: Enhanced IBRS");
    EXPECT_EQ(spectre(SPEC_BPB_ENABLED | SPEC_RETPOLINE_ENABLED), "Mitigation: Retpolines");
    EXPECT_EQ(spectre(SPEC_BPB_ENABLED), "Mitigation: IBRS");
    EXPECT_EQ(spectre(SPEC_BPB_DISABLED_SYSTEM_POLICY), "Vulnerable: the mitigation is turned off by system policy");
    EXPECT_EQ(spectre(SPEC_BPB_DISABLED_NO_HARDWARE_SUPPORT), "Vulnerable: the CPU's microcode doesn't support the mitigation");
    EXPECT_EQ(spectre(0), "Vulnerable");
}

TEST(WindowsPlatformSecurityTest, DecodesStoreBypassAndMds)
{
    using namespace WindowsPlatformSecurity;
    const auto find = [](const std::vector<CpuVulnerability>& list, std::string_view name) -> std::string
    {
        for (const CpuVulnerability& item : list)
        {
            if (item.name == name)
            {
                return item.status;
            }
        }
        return "(absent)";
    };
    EXPECT_EQ(find(decodeVulnerabilities(0, std::nullopt, ""), "spec_store_bypass"), "(absent)"); // not reported
    EXPECT_EQ(find(decodeVulnerabilities(SPEC_SSBD_AVAILABLE, std::nullopt, ""), "spec_store_bypass"), "Not affected");
    EXPECT_EQ(find(decodeVulnerabilities(SPEC_SSBD_AVAILABLE | SPEC_SSBD_REQUIRED | SPEC_SSBD_SYSTEM_WIDE, std::nullopt, ""),
                   "spec_store_bypass"),
              "Mitigation: Speculative Store Bypass disabled system-wide");

    EXPECT_EQ(find(decodeVulnerabilities(0, std::nullopt, "GenuineIntel"), "mds"), "(absent)"); // too old to report
    EXPECT_EQ(find(decodeVulnerabilities(SPEC_MB_CLEAR_REPORTED | SPEC_MB_CLEAR_ENABLED, std::nullopt, "GenuineIntel"), "mds"),
              "Mitigation: Clear CPU buffers");
    EXPECT_EQ(find(decodeVulnerabilities(SPEC_MB_CLEAR_REPORTED, std::nullopt, "GenuineIntel"), "mds"),
              "Vulnerable: Clear CPU buffers is off");
    EXPECT_EQ(find(decodeVulnerabilities(SPEC_MB_CLEAR_REPORTED, std::nullopt, "AuthenticAMD"), "mds"), "Not affected");
    EXPECT_EQ(find(decodeVulnerabilities(SPEC_MB_CLEAR_REPORTED, std::nullopt, ""), "mds"), "(absent)"); // no guess
}

TEST(WindowsPlatformSecurityTest, DecodesKernelVaShadow)
{
    using namespace WindowsPlatformSecurity;
    const auto decode = [](std::uint32_t kva, std::string_view vendor)
    {
        const auto list = decodeVulnerabilities(std::nullopt, kva, vendor);
        std::map<std::string, std::string> byName;
        for (const CpuVulnerability& item : list)
        {
            byName[item.name] = item.status;
        }
        return byName;
    };
    const auto shadowed =
        decode(KVA_SHADOW_REQUIRED_AVAILABLE | KVA_SHADOW_REQUIRED | KVA_SHADOW_ENABLED | KVA_L1TF_MITIGATION_PRESENT, "");
    EXPECT_EQ(shadowed.at("meltdown"), "Mitigation: Kernel VA shadow");
    EXPECT_EQ(shadowed.at("l1tf"), "Mitigation: Invalid PTE bit");

    const auto off = decode(KVA_SHADOW_REQUIRED_AVAILABLE | KVA_SHADOW_REQUIRED, "");
    EXPECT_EQ(off.at("meltdown"), "Vulnerable: Kernel VA shadow is off");
    EXPECT_EQ(off.at("l1tf"), "Vulnerable");

    // Without the "required" bit being valid, the vendor decides; an unknown vendor gets no rows.
    EXPECT_EQ(decode(KVA_SHADOW_ENABLED, "GenuineIntel").at("meltdown"), "Mitigation: Kernel VA shadow");
    EXPECT_EQ(decode(0, "AuthenticAMD").at("meltdown"), "Not affected");
    EXPECT_TRUE(decode(0, "").empty());
}

} // namespace
} // namespace Platform
