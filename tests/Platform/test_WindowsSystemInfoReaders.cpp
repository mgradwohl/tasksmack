/// @file test_WindowsSystemInfoReaders.cpp
/// @brief The Windows halves of the Security (#1514), Sensors (#1522) and Boot performance (#1525) readers,
/// through fake function tables: the boot event parser and its errors (WindowsBootPerformance.h), the
/// thermal zone conversion and PDH statuses (WindowsThermalZones.h), and Secure Boot, the TPM, HVCI, Kernel
/// DMA Protection and the speculation-control decoding (WindowsPlatformSecurity.h). Standard library only,
/// so it runs everywhere.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WindowsBootPerformance.h"
#include "Platform/Windows/WindowsPlatformSecurity.h"
#include "Platform/Windows/WindowsThermalZones.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
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

// ========== Security (#1514) ==========

struct FakeSecurity
{
    std::optional<bool> uefi = true;
    std::optional<std::uint32_t> secureBoot = 1;
    std::optional<std::uint32_t> tbsResult = WindowsPlatformSecurity::TBS_SUCCESS_CODE;
    std::uint32_t tpmVersion = WindowsPlatformSecurity::TPM_VERSION_20_CODE;
    /// NtQuerySystemInformation's answers by class: the bytes written, or absent for STATUS_INVALID_INFO_CLASS.
    std::vector<std::pair<std::uint32_t, std::vector<std::byte>>> answers;
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
    };
}

/// The flags this machine's Windows 11 (Intel Core Ultra 7 255H) reports: both speculation-control words,
/// the second with MMIO, GDS, SRSO and RFDS all hardware-immune.
[[nodiscard]] FakeSecurity modernIntel()
{
    FakeSecurity fake;
    answer(fake, WindowsPlatformSecurity::SYSTEM_CODE_INTEGRITY_INFORMATION, words({8, 0xF401U}));
    answer(fake, WindowsPlatformSecurity::SYSTEM_DMA_GUARD_POLICY_INFORMATION, std::vector<std::byte>{std::byte{1}});
    answer(fake, WindowsPlatformSecurity::SYSTEM_SPECULATION_CONTROL_INFORMATION, words({0x7D8F'93F9U, 0x0B5A'BE57U}));
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
    ASSERT_EQ(info.vulnerabilities.size(), 9U);
    EXPECT_EQ(info.vulnerabilities.front().name, "gather_data_sampling"); // sorted by name
    EXPECT_EQ(statusOf(info, "spectre_v2"), "Mitigation: Enhanced IBRS");
    EXPECT_EQ(statusOf(info, "spec_store_bypass"), "Vulnerable: Speculative Store Bypass Disable is off (the Windows default)");
    EXPECT_EQ(statusOf(info, "mds"), "Not affected");
    EXPECT_EQ(statusOf(info, "meltdown"), "Not affected");
    EXPECT_EQ(statusOf(info, "l1tf"), "Not affected");
    EXPECT_EQ(statusOf(info, "mmio_stale_data"), "Not affected");
    EXPECT_EQ(statusOf(info, "gather_data_sampling"), "Not affected");
    EXPECT_EQ(statusOf(info, "spec_rstack_overflow"), "Not affected");
    EXPECT_EQ(statusOf(info, "reg_file_data_sampling"), "Not affected");
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

/// One decoding case: the speculation-control words (nullopt when the class isn't supported), the VA
/// shadow flags (likewise), and the status a vulnerability should read, "(absent)" for no row.
struct DecodeCase
{
    std::string_view what;
    std::optional<WindowsPlatformSecurity::SpeculationControlInformation> speculation;
    std::optional<std::uint32_t> kvaShadow;
    std::string_view name;
    std::string_view expected;
};

[[nodiscard]] DecodeCase when(std::string_view what,
                              std::optional<WindowsPlatformSecurity::SpeculationControlInformation> speculation,
                              std::optional<std::uint32_t> kvaShadow,
                              std::string_view name,
                              std::string_view expected)
{
    return {.what = what, .speculation = speculation, .kvaShadow = kvaShadow, .name = name, .expected = expected};
}

[[nodiscard]] std::string decodedStatus(const DecodeCase& item)
{
    for (const CpuVulnerability& vulnerability : WindowsPlatformSecurity::decodeVulnerabilities(item.speculation, item.kvaShadow))
    {
        if (vulnerability.name == item.name)
        {
            return vulnerability.status;
        }
    }
    return "(absent)";
}

void expectDecodes(std::span<const DecodeCase> cases)
{
    std::size_t index = 0;
    while (index < cases.size())
    {
        const DecodeCase& item = cases[index];
        EXPECT_EQ(decodedStatus(item), item.expected) << item.what;
        ++index;
    }
}

/// SpeculationControlInformation's two words; a build before 23H2 leaves the second zero.
[[nodiscard]] std::optional<WindowsPlatformSecurity::SpeculationControlInformation> spec(std::uint32_t flags, std::uint32_t flags2 = 0)
{
    return WindowsPlatformSecurity::SpeculationControlInformation{.flags = flags, .flags2 = flags2};
}

TEST(WindowsPlatformSecurityTest, DecodesSpectreV2)
{
    using namespace WindowsPlatformSecurity;
    constexpr std::string_view NO_MICROCODE = "Vulnerable: the CPU's microcode doesn't support the mitigation";
    const std::array cases{
        when("enhanced IBRS wins",
             spec(SPEC_BPB_ENABLED | SPEC_ENHANCED_IBRS | SPEC_RETPOLINE_ENABLED),
             0,
             "spectre_v2",
             "Mitigation: Enhanced IBRS"),
        when("retpolines", spec(SPEC_BPB_ENABLED | SPEC_RETPOLINE_ENABLED | SPEC_IBRS_PRESENT), 0, "spectre_v2", "Mitigation: Retpolines"),
        when("IBRS", spec(SPEC_BPB_ENABLED | SPEC_IBRS_PRESENT), 0, "spectre_v2", "Mitigation: IBRS"),
        when("barriers only", spec(SPEC_BPB_ENABLED), 0, "spectre_v2", "Mitigation: Branch prediction barriers"),
        when("policy",
             spec(SPEC_BPB_DISABLED_SYSTEM_POLICY | SPEC_IBRS_PRESENT),
             0,
             "spectre_v2",
             "Vulnerable: the mitigation is turned off by system policy"),
        when("no hardware bit", spec(SPEC_BPB_DISABLED_NO_HARDWARE_SUPPORT | SPEC_SPEC_CTRL_ENUMERATED), 0, "spectre_v2", NO_MICROCODE),
        when("no SPEC_CTRL at all", spec(0), 0, "spectre_v2", NO_MICROCODE),
        when("SPEC_CTRL enumerated, off", spec(SPEC_SPEC_CTRL_ENUMERATED), 0, "spectre_v2", "Vulnerable"),
        when("SPEC_CMD enumerated, off", spec(SPEC_SPEC_CMD_ENUMERATED), 0, "spectre_v2", "Vulnerable"),
        when("STIBP present, off", spec(SPEC_STIBP_PRESENT), 0, "spectre_v2", "Vulnerable"),
        when("class unsupported", std::nullopt, KVA_SHADOW_REQUIRED_AVAILABLE, "spectre_v2", "Unknown"),
    };
    expectDecodes(cases);
}

TEST(WindowsPlatformSecurityTest, DecodesStoreBypass)
{
    using namespace WindowsPlatformSecurity;
    const std::array cases{
        when("Windows without SSBD", spec(SPEC_SSBD_REQUIRED), 0, "spec_store_bypass", "Unknown"),
        when("not required", spec(SPEC_SSBD_AVAILABLE | SPEC_SSBD_SUPPORTED), 0, "spec_store_bypass", "Not affected"),
        when("on system-wide",
             spec(SPEC_SSBD_AVAILABLE | SPEC_SSBD_SUPPORTED | SPEC_SSBD_REQUIRED | SPEC_SSBD_SYSTEM_WIDE),
             0,
             "spec_store_bypass",
             "Mitigation: Speculative Store Bypass disabled system-wide"),
        when("off (default)",
             spec(SPEC_SSBD_AVAILABLE | SPEC_SSBD_SUPPORTED | SPEC_SSBD_REQUIRED),
             0,
             "spec_store_bypass",
             "Vulnerable: Speculative Store Bypass Disable is off (the Windows default)"),
        when("no microcode",
             spec(SPEC_SSBD_AVAILABLE | SPEC_SSBD_REQUIRED),
             0,
             "spec_store_bypass",
             "Vulnerable: the CPU's microcode doesn't support Speculative Store Bypass Disable"),
        when("class unsupported", std::nullopt, 0, "spec_store_bypass", "Unknown"),
    };
    expectDecodes(cases);
}

TEST(WindowsPlatformSecurityTest, DecodesMdsAndMmioStaleData)
{
    using namespace WindowsPlatformSecurity;
    constexpr std::uint32_t ALL_PROTECTED =
        SPEC2_SBDR_SSDP_HARDWARE_PROTECTED | SPEC2_FBSDP_HARDWARE_PROTECTED | SPEC2_PSDP_HARDWARE_PROTECTED;
    const std::array cases{
        when("MDS: too old to report", spec(SPEC_MB_CLEAR_ENABLED), 0, "mds", "Unknown"),
        when("MDS: protected", spec(SPEC_MB_CLEAR_REPORTED | SPEC_MDS_HARDWARE_PROTECTED), 0, "mds", "Not affected"),
        when("MDS: cleared", spec(SPEC_MB_CLEAR_REPORTED | SPEC_MB_CLEAR_ENABLED), 0, "mds", "Mitigation: Clear CPU buffers"),
        // Neither: an AMD or ARM CPU Windows doesn't flag, or an Intel one with the mitigation off; no guess.
        when("MDS: neither", spec(SPEC_MB_CLEAR_REPORTED), 0, "mds", "Unknown"),
        when("MDS: class unsupported", std::nullopt, 0, "mds", "Unknown"),
        when("MMIO: before 23H2", spec(0), 0, "mmio_stale_data", "(absent)"),
        when("MMIO: second word without FbClearReported", spec(0, ALL_PROTECTED), 0, "mmio_stale_data", "(absent)"),
        when("MMIO: all three protected", spec(0, SPEC2_FB_CLEAR_REPORTED | ALL_PROTECTED), 0, "mmio_stale_data", "Not affected"),
        when("MMIO: cleared",
             spec(0, SPEC2_FB_CLEAR_REPORTED | SPEC2_SBDR_SSDP_HARDWARE_PROTECTED | SPEC2_FB_CLEAR_ENABLED),
             0,
             "mmio_stale_data",
             "Mitigation: Clear CPU buffers"),
        when("MMIO: two of three protected, not cleared",
             spec(0, SPEC2_FB_CLEAR_REPORTED | SPEC2_SBDR_SSDP_HARDWARE_PROTECTED | SPEC2_FBSDP_HARDWARE_PROTECTED),
             0,
             "mmio_stale_data",
             "Unknown"),
        when("MMIO: class unsupported", std::nullopt, 0, "mmio_stale_data", "(absent)"),
    };
    expectDecodes(cases);
}

TEST(WindowsPlatformSecurityTest, DecodesMeltdown)
{
    using namespace WindowsPlatformSecurity;
    const std::array cases{
        when("required, shadowed",
             spec(0),
             KVA_SHADOW_REQUIRED_AVAILABLE | KVA_SHADOW_REQUIRED | KVA_SHADOW_ENABLED,
             "meltdown",
             "Mitigation: Kernel VA shadow"),
        when("required, off",
             spec(0),
             KVA_SHADOW_REQUIRED_AVAILABLE | KVA_SHADOW_REQUIRED,
             "meltdown",
             "Vulnerable: Kernel VA shadow is off"),
        when("not required", spec(0), KVA_SHADOW_REQUIRED_AVAILABLE, "meltdown", "Not affected"),
        // Before KvaShadowRequiredAvailable, Windows didn't say; the vendor no longer decides.
        when("old build, shadowed", spec(0), KVA_SHADOW_ENABLED, "meltdown", "Unknown"),
        when("old build, not shadowed", spec(0), 0, "meltdown", "Unknown"),
        when("RDCL protected (23H2)",
             spec(0, SPEC2_RDCL_HARDWARE_PROTECTED_REPORTED | SPEC2_RDCL_HARDWARE_PROTECTED),
             0,
             "meltdown",
             "Not affected"),
        when("RDCL not protected, shadowed",
             spec(0, SPEC2_RDCL_HARDWARE_PROTECTED_REPORTED),
             KVA_SHADOW_ENABLED,
             "meltdown",
             "Mitigation: Kernel VA shadow"),
        when(
            "RDCL not protected, no VA shadow class", spec(0, SPEC2_RDCL_HARDWARE_PROTECTED_REPORTED), std::nullopt, "meltdown", "Unknown"),
        when("VA shadow class unsupported", spec(0), std::nullopt, "meltdown", "Unknown"),
    };
    expectDecodes(cases);
}

TEST(WindowsPlatformSecurityTest, DecodesL1tf)
{
    using namespace WindowsPlatformSecurity;
    constexpr std::uint32_t PTE_BIT = 0x0B40U; // InvalidPteBit 45
    constexpr std::uint32_t REQUIRED = KVA_SHADOW_REQUIRED_AVAILABLE | KVA_SHADOW_REQUIRED;
    const std::array cases{
        when("no KVA shadowing needed", spec(0), KVA_SHADOW_REQUIRED_AVAILABLE, "l1tf", "Not affected"),
        when("RDCL protected", spec(0, SPEC2_RDCL_HARDWARE_PROTECTED_REPORTED | SPEC2_RDCL_HARDWARE_PROTECTED), 0, "l1tf", "Not affected"),
        when("hypervisor says not affected",
             spec(SPEC_HV_L1TF_STATUS_AVAILABLE | SPEC_HV_L1TF_PROCESSOR_NOT_AFFECTED),
             REQUIRED | KVA_SHADOW_ENABLED,
             "l1tf",
             "Not affected"),
        // KVA shadowing needed doesn't say L1TF is (Goldmont needs one, not the other), so without the
        // hypervisor's word it is Unknown.
        when(
            "KVA shadowing needed only", spec(0), REQUIRED | KVA_SHADOW_ENABLED | PTE_BIT | KVA_L1TF_MITIGATION_PRESENT, "l1tf", "Unknown"),
        when("affected, invalid PTE bit",
             spec(SPEC_HV_L1TF_STATUS_AVAILABLE),
             REQUIRED | KVA_SHADOW_ENABLED | PTE_BIT,
             "l1tf",
             "Mitigation: Invalid PTE bit"),
        when("affected, present but off",
             spec(SPEC_HV_L1TF_STATUS_AVAILABLE),
             REQUIRED | KVA_L1TF_MITIGATION_PRESENT,
             "l1tf",
             "Vulnerable: the invalid PTE bit is off"),
        when(
            "affected, no mitigation", spec(SPEC_HV_L1TF_STATUS_AVAILABLE), REQUIRED, "l1tf", "Vulnerable: Windows has no L1TF mitigation"),
        when("affected, no VA shadow class", spec(SPEC_HV_L1TF_STATUS_AVAILABLE), std::nullopt, "l1tf", "Unknown"),
        when("old build", spec(0), 0, "l1tf", "Unknown"),
    };
    expectDecodes(cases);
}

TEST(WindowsPlatformSecurityTest, DecodesStatusFields)
{
    using namespace WindowsPlatformSecurity;
    const auto gds = [](std::uint32_t status)
    {
        return spec(0, SPEC2_GDS_REPORTED | (status << SPEC2_GDS_STATUS_SHIFT));
    };
    const auto srso = [](std::uint32_t status)
    {
        return spec(0, SPEC2_SRSO_REPORTED | (status << SPEC2_SRSO_STATUS_SHIFT));
    };
    const auto rfds = [](std::uint32_t status)
    {
        return spec(0, SPEC2_RFDS_REPORTED | (status << SPEC2_RFDS_STATUS_SHIFT));
    };
    const std::array cases{
        when("GDS: not reported", spec(0, 3U << SPEC2_GDS_STATUS_SHIFT), 0, "gather_data_sampling", "(absent)"),
        when("GDS: unsupported", gds(MITIGATION_STATUS_UNSUPPORTED), 0, "gather_data_sampling", "Unknown"),
        when("GDS: disabled", gds(MITIGATION_STATUS_DISABLED), 0, "gather_data_sampling", "Vulnerable"),
        when("GDS: immune", gds(MITIGATION_STATUS_HARDWARE_IMMUNE), 0, "gather_data_sampling", "Not affected"),
        when("GDS: mitigated", gds(MITIGATION_STATUS_MITIGATED), 0, "gather_data_sampling", "Mitigation: Microcode"),
        when("GDS: locked", gds(MITIGATION_STATUS_MITIGATED_AND_LOCKED), 0, "gather_data_sampling", "Mitigation: Microcode (locked)"),
        when("GDS: a value newer than this decoder", gds(7), 0, "gather_data_sampling", "Unknown"),
        when("SRSO: not reported", spec(0), 0, "spec_rstack_overflow", "(absent)"),
        when("SRSO: unsupported", srso(MITIGATION_STATUS_UNSUPPORTED), 0, "spec_rstack_overflow", "Unknown"),
        when("SRSO: disabled", srso(MITIGATION_STATUS_DISABLED), 0, "spec_rstack_overflow", "Vulnerable"),
        when("SRSO: immune", srso(MITIGATION_STATUS_HARDWARE_IMMUNE), 0, "spec_rstack_overflow", "Not affected"),
        when("SRSO: mitigated", srso(MITIGATION_STATUS_MITIGATED), 0, "spec_rstack_overflow", "Mitigation: Windows SRSO mitigation"),
        when("RFDS: not reported", spec(0), 0, "reg_file_data_sampling", "(absent)"),
        when("RFDS: unsupported", rfds(MITIGATION_STATUS_UNSUPPORTED), 0, "reg_file_data_sampling", "Unknown"),
        when("RFDS: disabled", rfds(MITIGATION_STATUS_DISABLED), 0, "reg_file_data_sampling", "Vulnerable"),
        when("RFDS: immune", rfds(MITIGATION_STATUS_HARDWARE_IMMUNE), 0, "reg_file_data_sampling", "Not affected"),
        when("RFDS: mitigated", rfds(MITIGATION_STATUS_MITIGATED), 0, "reg_file_data_sampling", "Mitigation: Clear Register File"),
    };
    expectDecodes(cases);
}

TEST(WindowsPlatformSecurityTest, ReadsTheSecondWordOnlyWhereWindowsHasIt)
{
    using namespace WindowsPlatformSecurity;
    // A build before 23H2: the class wants exactly one word, so the two-word query fails and the reader
    // asks again for one. The core rows stay; the second word's rows are left out.
    FakeSecurity older = modernIntel();
    answer(older, SYSTEM_SPECULATION_CONTROL_INFORMATION, words({0x7D8F'93F9U}));
    PlatformSecurityInfo olderInfo;
    readPlatformSecurity(olderInfo, fakeSecurityFunctions(older));
    ASSERT_EQ(olderInfo.vulnerabilities.size(), 5U);
    EXPECT_EQ(statusOf(olderInfo, "spectre_v2"), "Mitigation: Enhanced IBRS");
    EXPECT_EQ(statusOf(olderInfo, "mds"), "Not affected");
    EXPECT_EQ(statusOf(olderInfo, "meltdown"), "Not affected");
    EXPECT_EQ(statusOf(olderInfo, "mmio_stale_data"), "(absent)");

    // An older build still: SSBD and MbClear not yet reported, and KvaShadowRequired not yet available.
    FakeSecurity rs3 = modernIntel();
    answer(rs3, SYSTEM_SPECULATION_CONTROL_INFORMATION, words({SPEC_BPB_ENABLED | SPEC_IBRS_PRESENT}));
    answer(rs3, SYSTEM_KERNEL_VA_SHADOW_INFORMATION, words({KVA_SHADOW_ENABLED}));
    PlatformSecurityInfo rs3Info;
    readPlatformSecurity(rs3Info, fakeSecurityFunctions(rs3));
    ASSERT_EQ(rs3Info.vulnerabilities.size(), 5U);
    EXPECT_EQ(statusOf(rs3Info, "spectre_v2"), "Mitigation: IBRS");
    EXPECT_EQ(statusOf(rs3Info, "spec_store_bypass"), "Unknown");
    EXPECT_EQ(statusOf(rs3Info, "mds"), "Unknown");
    EXPECT_EQ(statusOf(rs3Info, "meltdown"), "Unknown");
    EXPECT_EQ(statusOf(rs3Info, "l1tf"), "Unknown");
}

TEST(WindowsPlatformSecurityTest, OneMissingClassLeavesItsRowsUnknown)
{
    using namespace WindowsPlatformSecurity;
    FakeSecurity noSpeculation = modernIntel();
    std::erase_if(noSpeculation.answers, [](const auto& item) { return item.first == SYSTEM_SPECULATION_CONTROL_INFORMATION; });
    PlatformSecurityInfo noSpeculationInfo;
    readPlatformSecurity(noSpeculationInfo, fakeSecurityFunctions(noSpeculation));
    EXPECT_TRUE(noSpeculationInfo.vulnerabilitiesRead);
    ASSERT_EQ(noSpeculationInfo.vulnerabilities.size(), 5U);
    EXPECT_EQ(statusOf(noSpeculationInfo, "spectre_v2"), "Unknown");
    EXPECT_EQ(statusOf(noSpeculationInfo, "spec_store_bypass"), "Unknown");
    EXPECT_EQ(statusOf(noSpeculationInfo, "mds"), "Unknown");
    EXPECT_EQ(statusOf(noSpeculationInfo, "meltdown"), "Not affected"); // the VA shadow class still says
    EXPECT_EQ(statusOf(noSpeculationInfo, "l1tf"), "Not affected");

    FakeSecurity noKvaShadow = modernIntel();
    std::erase_if(noKvaShadow.answers, [](const auto& item) { return item.first == SYSTEM_KERNEL_VA_SHADOW_INFORMATION; });
    answer(noKvaShadow, SYSTEM_SPECULATION_CONTROL_INFORMATION, words({0x7D8F'93F9U}));
    PlatformSecurityInfo noKvaShadowInfo;
    readPlatformSecurity(noKvaShadowInfo, fakeSecurityFunctions(noKvaShadow));
    EXPECT_EQ(statusOf(noKvaShadowInfo, "meltdown"), "Unknown");
    EXPECT_EQ(statusOf(noKvaShadowInfo, "l1tf"), "Not affected"); // the hypervisor's L1TF status says
    EXPECT_EQ(statusOf(noKvaShadowInfo, "spectre_v2"), "Mitigation: Enhanced IBRS");
}

} // namespace
} // namespace Platform
