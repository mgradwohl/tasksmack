/// @file test_CrashesInfo.cpp
/// @brief The Recent crashes & hangs readers (#1524): the Application log event parser and the Windows
/// reader through a fake function table (Platform/Windows/WindowsCrashEvents.h), and the systemd-coredump
/// file name parser and the Linux reader under a fixture root (Platform/Linux/LinuxCoredumps.h). Standard
/// library only, so it runs everywhere.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxCoredumps.h"
#include "Platform/Windows/WindowsCrashEvents.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform
{
namespace
{

// An Application Error event as EvtRender(EvtRenderEventXml) renders it on Windows 11: named data.
constexpr std::string_view APP_ERROR_XML =
    "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System>"
    "<Provider Name='Application Error' Guid='{a0e9b465-b939-57d7-b27d-95d8e925ff57}'/><EventID>1000</EventID>"
    "<Version>0</Version><Level>2</Level><Task>100</Task><Opcode>0</Opcode><Keywords>0x8000000000000000</Keywords>"
    "<TimeCreated SystemTime='2026-10-01T14:03:22.1234567Z'/><EventRecordID>48211</EventRecordID><Correlation/>"
    "<Execution ProcessID='15928' ThreadID='9304'/><Channel>Application</Channel><Computer>DESKTOP-TEST</Computer>"
    "<Security UserID='S-1-5-21-1-2-3-1001'/></System><EventData>"
    "<Data Name='AppName'>Contoso &amp; Co.exe</Data><Data Name='AppVersion'>2.4.0.17</Data>"
    "<Data Name='AppTimeStamp'>66f1c2a0</Data><Data Name='ModuleName'>ntdll.dll</Data>"
    "<Data Name='ModuleVersion'>10.0.26100.2033</Data><Data Name='ModuleTimeStamp'>5e1a2b3c</Data>"
    "<Data Name='ExceptionCode'>c0000005</Data><Data Name='FaultingOffset'>000000000002f1a3</Data>"
    "<Data Name='ProcessId'>0x1a2c</Data><Data Name='ProcessCreationTime'>0x1db13f0a2c4e8f1</Data>"
    "<Data Name='AppPath'>C:\\Program Files\\Contoso\\Contoso &amp; Co.exe</Data>"
    "<Data Name='ModulePath'>C:\\Windows\\SYSTEM32\\ntdll.dll</Data>"
    "<Data Name='IntegratorReportId'>6d1c8f0e-1b2a-4c3d-9e8f-0a1b2c3d4e5f</Data><Data Name='PackageFullName'/>"
    "<Data Name='PackageRelativeAppId'></Data></EventData></Event>";

// An Application Hang event as Windows 10 renders it: unnamed data in order, the hang type last.
constexpr std::string_view APP_HANG_XML =
    "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System>"
    "<Provider Name='Application Hang'/><EventID Qualifiers='0'>1002</EventID><Version>0</Version><Level>2</Level>"
    "<Task>101</Task><TimeCreated SystemTime='2026-10-03T08:15:00.0000000Z'/><EventRecordID>48300</EventRecordID>"
    "<Channel>Application</Channel><Computer>DESKTOP-TEST</Computer><Security/></System><EventData>"
    "<Data>notepad.exe</Data><Data>11.2408.12.0</Data><Data>3f60</Data><Data>01db15a2c0ffee00</Data>"
    "<Data>4294967295</Data><Data>C:\\Program Files\\WindowsApps\\Notepad\\notepad.exe</Data>"
    "<Data>0b6d1f2e-3c4d-5e6f-7a8b-9c0d1e2f3a4b</Data><Data>Microsoft.WindowsNotepad_11.2408.12.0_x64__8wekyb3d8bbwe</Data>"
    "<Data>App</Data><Data>Quiesce</Data></EventData><Binary>55006E006B006E006F0077006E0000000000</Binary></Event>";

// An Application Hang event as Windows 11 renders it: named data.
constexpr std::string_view NAMED_HANG_XML =
    "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System>"
    "<Provider Name='Application Hang' Guid='{c631c3dc-c676-59e4-2db3-5c0af00f9675}'/><EventID>1002</EventID>"
    "<Version>0</Version><Level>2</Level><Task>101</Task><TimeCreated SystemTime='2026-10-09T23:20:10.1941859Z'/>"
    "<Channel>Application</Channel><Computer>DESKTOP-TEST</Computer><Security UserID='S-1-5-21-1-2-3-1001'/></System>"
    "<EventData><Data Name='AppName'>contoso_proxy.exe</Data><Data Name='AppVersion'>154.0.4258.62</Data>"
    "<Data Name='ProcessId'>0x6854</Data><Data Name='StartTime'>0x1dd56d845b102cf</Data>"
    "<Data Name='TerminationTime'>4294967295</Data><Data Name='ExeFileName'>C:\\Program Files\\Contoso\\contoso_proxy.exe</Data>"
    "<Data Name='ReportId'>e29dc217-5fa6-4f64-a5a2-662b330a2a96</Data><Data Name='PackageFullName'></Data>"
    "<Data Name='PackageRelativeAppId'></Data><Data Name='HangType'>Cross-thread</Data></EventData></Event>";

TEST(WindowsCrashEventsTest, ParsesANamedApplicationHang)
{
    const std::optional<CrashEvent> hangRead = WindowsCrashEvents::parseEventXml(NAMED_HANG_XML);
    ASSERT_TRUE(hangRead.has_value());
    const auto hang = hangRead.value_or(CrashEvent{});
    EXPECT_TRUE(hang.hang);
    EXPECT_EQ(hang.application, "contoso_proxy.exe");
    EXPECT_EQ(hang.appVersion, "154.0.4258.62");
    EXPECT_EQ(hang.pid, std::optional<std::uint32_t>(0x6854));
    EXPECT_EQ(hang.hangType, "Cross-thread");
}

TEST(WindowsCrashEventsTest, ParsesAnApplicationError)
{
    const std::optional<CrashEvent> eventRead = WindowsCrashEvents::parseEventXml(APP_ERROR_XML);
    ASSERT_TRUE(eventRead.has_value());
    const auto event = eventRead.value_or(decltype(eventRead)::value_type{});
    EXPECT_FALSE(event.hang);
    EXPECT_EQ(event.unixSeconds, 1'790'863'402U); // 2026-10-01 14:03:22 UTC
    EXPECT_EQ(event.application, "Contoso & Co.exe");
    EXPECT_EQ(event.appVersion, "2.4.0.17");
    EXPECT_EQ(event.module, "ntdll.dll");
    EXPECT_EQ(event.moduleVersion, "10.0.26100.2033");
    EXPECT_EQ(event.exceptionCode, "0xC0000005");
    EXPECT_EQ(event.pid, std::optional<std::uint32_t>(0x1a2c));
    EXPECT_TRUE(event.hangType.empty());
    EXPECT_FALSE(event.uid.has_value());
}

TEST(WindowsCrashEventsTest, ParsesAnApplicationHang)
{
    const std::optional<CrashEvent> eventRead = WindowsCrashEvents::parseEventXml(APP_HANG_XML);
    ASSERT_TRUE(eventRead.has_value());
    const auto event = eventRead.value_or(decltype(eventRead)::value_type{});
    EXPECT_TRUE(event.hang);
    EXPECT_EQ(event.unixSeconds, 1'791'015'300U); // 2026-10-03 08:15:00 UTC
    EXPECT_EQ(event.application, "notepad.exe");
    EXPECT_EQ(event.appVersion, "11.2408.12.0");
    EXPECT_EQ(event.pid, std::optional<std::uint32_t>(0x3f60));
    EXPECT_EQ(event.hangType, "Quiesce");
    EXPECT_TRUE(event.module.empty());
    EXPECT_TRUE(event.exceptionCode.empty());
}

TEST(WindowsCrashEventsTest, MissingFieldsAndOlderPositionalData)
{
    // An older Application Error with unnamed data, and no ProcessId or time.
    const std::optional<CrashEvent> positionalRead =
        WindowsCrashEvents::parseEventXml("<Event><System><Provider Name=\"Application Error\"/><EventID>1000</EventID></System><EventData>"
                                          "<Data>legacy.exe</Data><Data>1.0.0.0</Data><Data>4a5b6c7d</Data><Data>KERNELBASE.dll</Data>"
                                          "<Data>10.0.19041.1</Data><Data>0</Data><Data>e0434352</Data></EventData></Event>");
    ASSERT_TRUE(positionalRead.has_value());
    const auto positional = positionalRead.value_or(decltype(positionalRead)::value_type{});
    EXPECT_EQ(positional.application, "legacy.exe");
    EXPECT_EQ(positional.module, "KERNELBASE.dll");
    EXPECT_EQ(positional.exceptionCode, "0xE0434352");
    EXPECT_EQ(positional.unixSeconds, 0U);
    EXPECT_FALSE(positional.pid.has_value());

    // A hang with only the application, and a crash whose data is all empty.
    const std::optional<CrashEvent> sparseRead =
        WindowsCrashEvents::parseEventXml("<Event><System><EventID>1002</EventID><TimeCreated "
                                          "SystemTime='bogus'/></System><EventData><Data>a.exe</Data></EventData></Event>");
    ASSERT_TRUE(sparseRead.has_value());
    const auto sparse = sparseRead.value_or(decltype(sparseRead)::value_type{});
    EXPECT_EQ(sparse.application, "a.exe");
    EXPECT_TRUE(sparse.hangType.empty());
    EXPECT_EQ(sparse.unixSeconds, 0U);
    const std::optional<CrashEvent> emptyRead =
        WindowsCrashEvents::parseEventXml("<Event><System><EventID>1000</EventID></System></Event>");
    ASSERT_TRUE(emptyRead.has_value());
    const auto empty = emptyRead.value_or(decltype(emptyRead)::value_type{});
    EXPECT_TRUE(empty.application.empty());
    EXPECT_TRUE(empty.exceptionCode.empty());
}

TEST(WindowsCrashEventsTest, RejectsOtherEvents)
{
    using WindowsCrashEvents::parseEventXml;
    EXPECT_FALSE(parseEventXml("").has_value());
    EXPECT_FALSE(parseEventXml("<Event><System><EventID>1001</EventID></System></Event>").has_value());
    EXPECT_FALSE(parseEventXml("<Event><System><Provider Name='MsiInstaller'/><EventID>1000</EventID></System></Event>").has_value());
    EXPECT_FALSE(parseEventXml("<Event><System><Provider Name='Application Error'/><EventID>1002</EventID></System></Event>").has_value());
    EXPECT_FALSE(parseEventXml("<Event><System><EventIDx>1000</EventIDx></System></Event>").has_value());
}

TEST(WindowsCrashEventsTest, Helpers)
{
    using namespace WindowsCrashEvents;
    EXPECT_EQ(unescapeXml("a &lt;b&gt; &quot;c&quot; &apos;d&apos; &amp;amp; &#65; &bogus; &"), "a <b> \"c\" 'd' &amp; &#65; &bogus; &");
    EXPECT_EQ(formatExceptionCode("c0000005"), "0xC0000005");
    EXPECT_EQ(formatExceptionCode("0x80000003"), "0x80000003");
    EXPECT_EQ(formatExceptionCode("409"), "0x00000409");
    EXPECT_EQ(formatExceptionCode("not hex"), "not hex");
    EXPECT_EQ(parseSystemTime("1970-01-02T00:00:00Z"), 86'400U);
    EXPECT_EQ(parseSystemTime("2026-02-30T00:00:00Z"), 0U);
    EXPECT_EQ(parseSystemTime("2026-10-01"), 0U);
    const std::string xpath = crashQueryXPath(CRASH_HISTORY_DAYS);
    EXPECT_NE(xpath.find("EventID=1000 or EventID=1002"), std::string::npos);
    EXPECT_NE(xpath.find("timediff(@SystemTime) <= 1209600000"), std::string::npos); // 14 days in ms
}

/// The fake table's state: the open error (0 opens) and the events it yields, newest first.
std::uint32_t g_OpenError = 0;     // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::vector<std::string> g_Events; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::size_t g_Next = 0;            // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
bool g_Closed = false;             // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::string g_XPath;               // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
int g_Handle = 0;                  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

[[nodiscard]] WindowsCrashEvents::Functions fakeFunctions(std::uint32_t openError, std::vector<std::string> events)
{
    g_OpenError = openError;
    g_Events = std::move(events);
    g_Next = 0;
    g_Closed = false;
    return {
        .openQuery = [](const std::string& xpath, std::uint32_t& error) -> void*
        {
            g_XPath = xpath;
            error = g_OpenError;
            return g_OpenError == 0 ? &g_Handle : nullptr;
        },
        .nextEventXml =
            [](void* /*query*/, std::string& xml)
        {
            if (g_Next >= g_Events.size())
            {
                return false;
            }
            xml = g_Events[g_Next++];
            return true;
        },
        .closeQuery = [](void* query) { g_Closed = query == &g_Handle; },
    };
}

TEST(WindowsCrashEventsTest, ReadsThroughTheTable)
{
    CrashesInfo info;
    WindowsCrashEvents::readCrashes(info, fakeFunctions(0, {std::string(APP_HANG_XML), "", std::string(APP_ERROR_XML)}));
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Windows);
    EXPECT_TRUE(info.listed);
    EXPECT_FALSE(info.capped);
    EXPECT_TRUE(g_Closed);
    EXPECT_NE(g_XPath.find("Application Hang"), std::string::npos);
    ASSERT_EQ(info.events.size(), 2U); // the unrenderable one is skipped
    EXPECT_TRUE(info.events[0].hang);  // newest first
    EXPECT_EQ(info.events[1].module, "ntdll.dll");
}

TEST(WindowsCrashEventsTest, CapsAtTheNewest)
{
    CrashesInfo info;
    WindowsCrashEvents::readCrashes(info, fakeFunctions(0, std::vector<std::string>(5, std::string(APP_ERROR_XML))), 3);
    EXPECT_TRUE(info.capped);
    EXPECT_EQ(info.events.size(), 3U);
    EXPECT_EQ(g_Next, 4U); // stopped one past the cap
    EXPECT_TRUE(g_Closed);

    CrashesInfo exact;
    WindowsCrashEvents::readCrashes(exact, fakeFunctions(0, std::vector<std::string>(3, std::string(APP_ERROR_XML))), 3);
    EXPECT_FALSE(exact.capped);
    EXPECT_EQ(exact.events.size(), 3U);
}

TEST(WindowsCrashEventsTest, AccessDeniedAndMissingLog)
{
    CrashesInfo denied;
    WindowsCrashEvents::readCrashes(denied, fakeFunctions(WindowsCrashEvents::ERROR_ACCESS_DENIED_CODE, {}));
    EXPECT_TRUE(denied.available);
    EXPECT_FALSE(denied.listed);
    EXPECT_TRUE(denied.accessDenied);
    EXPECT_EQ(denied.unavailableReason, "Reading the Application event log requires permission");
    EXPECT_FALSE(g_Closed); // nothing was opened

    CrashesInfo missing;
    WindowsCrashEvents::readCrashes(missing, fakeFunctions(WindowsCrashEvents::ERROR_EVT_CHANNEL_NOT_FOUND_CODE, {}));
    EXPECT_FALSE(missing.listed);
    EXPECT_FALSE(missing.accessDenied);
    EXPECT_EQ(missing.unavailableReason, "The Application event log isn't available (missing or disabled)");

    CrashesInfo other;
    WindowsCrashEvents::readCrashes(other, fakeFunctions(1722, {}));
    EXPECT_EQ(other.unavailableReason, "The Application event log couldn't be read (error 1722)");
}

constexpr std::string_view BOOT_ID = "0123456789abcdef0123456789abcdef";

TEST(LinuxCoredumpsTest, ParsesCoreFileNames)
{
    using LinuxCoredumps::parseCoredumpName;
    const auto zstRead = parseCoredumpName(std::format("core.python3\\x2e12.1000.{}.4242.1791468202123456.zst", BOOT_ID));
    ASSERT_TRUE(zstRead.has_value());
    const auto zst = zstRead.value_or(decltype(zstRead)::value_type{});
    EXPECT_EQ(zst.comm, "python3.12");
    EXPECT_EQ(zst.uid, 1000U);
    EXPECT_EQ(zst.bootId, BOOT_ID);
    EXPECT_EQ(zst.pid, 4242U);
    EXPECT_EQ(zst.usec, 1'791'468'202'123'456U);
    EXPECT_EQ(zst.compression, "zst");

    const auto plainRead = parseCoredumpName(std::format("core.Web\\x20Content.0.{}.7.1000000", BOOT_ID));
    ASSERT_TRUE(plainRead.has_value());
    const auto plain = plainRead.value_or(decltype(plainRead)::value_type{});
    EXPECT_EQ(plain.comm, "Web Content");
    EXPECT_EQ(plain.uid, 0U);
    EXPECT_TRUE(plain.compression.empty());

    // An unescaped dot in the comm (an older systemd) still parses from the right.
    const auto dottedRead = parseCoredumpName(std::format("core.a.b.1000.{}.1.2.xz", BOOT_ID));
    ASSERT_TRUE(dottedRead.has_value());
    const auto dotted = dottedRead.value_or(decltype(dottedRead)::value_type{});
    EXPECT_EQ(dotted.comm, "a.b");
    EXPECT_EQ(dotted.compression, "xz");
}

TEST(LinuxCoredumpsTest, RejectsOtherNames)
{
    using LinuxCoredumps::parseCoredumpName;
    EXPECT_FALSE(parseCoredumpName("").has_value());
    EXPECT_FALSE(parseCoredumpName("core").has_value());
    EXPECT_FALSE(parseCoredumpName(std::format(".#core.x.1000.{}.1.2.zst1a2b3c", BOOT_ID)).has_value()); // a temporary file
    EXPECT_FALSE(parseCoredumpName(std::format("core..1000.{}.1.2", BOOT_ID)).has_value());              // no comm
    EXPECT_FALSE(parseCoredumpName("core.x.1000.notabootid.1.2").has_value());
    EXPECT_FALSE(parseCoredumpName(std::format("core.x.uid.{}.1.2", BOOT_ID)).has_value());
    EXPECT_FALSE(parseCoredumpName(std::format("core.x.1000.{}.1.2.", BOOT_ID)).has_value());
    EXPECT_FALSE(parseCoredumpName(std::format("core.x.99999999999.{}.1.2", BOOT_ID)).has_value()); // uid overflow
    EXPECT_EQ(LinuxCoredumps::unescapeComm("a\\x2fb\\xZZ\\"), "a/b\\xZZ\\");
}

/// A fresh fixture root under the temp directory, removed (best effort) at the end.
class CoredumpsFixture : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        static std::atomic<unsigned> counter{0};
        m_Root = std::filesystem::temp_directory_path() /
                 std::format("ts_coredumps_{}_{}", counter.fetch_add(1), std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(m_Root);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_Root, ec);
    }

    void core(std::string_view comm, std::uint64_t unixSeconds, std::string_view bytes = "x") const
    {
        const auto dir = m_Root / LinuxCoredumps::COREDUMP_DIR;
        std::filesystem::create_directories(dir);
        std::ofstream out(dir / std::format("core.{}.1000.{}.42.{}000000.zst", comm, BOOT_ID, unixSeconds), std::ios::binary);
        out << bytes;
    }

    std::filesystem::path m_Root;
};

constexpr std::uint64_t NOW = 1'791'700'000;
constexpr std::uint64_t DAY = 86'400;

TEST_F(CoredumpsFixture, ListsRecentCoresNewestFirst)
{
    core("old", NOW - (15 * DAY)); // outside the 14 days
    core("first", NOW - (3 * DAY), "abcd");
    core("second", NOW - DAY);
    const auto dir = m_Root / LinuxCoredumps::COREDUMP_DIR;
    std::ofstream(dir / "README").put('x'); // not a core file
    CrashesInfo info;
    LinuxCoredumps::readCoredumps(m_Root, NOW, info);
    EXPECT_TRUE(info.available);
    EXPECT_EQ(info.family, OsFamily::Linux);
    EXPECT_TRUE(info.listed);
    EXPECT_FALSE(info.capped);
    ASSERT_EQ(info.events.size(), 2U);
    EXPECT_EQ(info.events[0].application, "second");
    EXPECT_EQ(info.events[0].unixSeconds, NOW - DAY);
    EXPECT_EQ(info.events[1].application, "first");
    EXPECT_EQ(info.events[1].coreBytes, 4U);
    EXPECT_EQ(info.events[1].pid, std::optional<std::uint32_t>(42));
    EXPECT_EQ(info.events[1].uid, std::optional<std::uint32_t>(1000));
}

TEST_F(CoredumpsFixture, CapsAtTheNewest)
{
    for (std::uint64_t i = 0; i < 5; ++i)
    {
        core(std::format("app{}", i), NOW - i);
    }
    CrashesInfo info;
    LinuxCoredumps::readCoredumps(m_Root, NOW, info, 3);
    EXPECT_TRUE(info.capped);
    ASSERT_EQ(info.events.size(), 3U);
    EXPECT_EQ(info.events[0].application, "app0");
    EXPECT_EQ(info.events[2].application, "app2");
}

TEST_F(CoredumpsFixture, NoCoredumpDirectory)
{
    CrashesInfo info;
    LinuxCoredumps::readCoredumps(m_Root, NOW, info);
    EXPECT_TRUE(info.available);
    EXPECT_FALSE(info.listed);
    EXPECT_FALSE(info.accessDenied);
    EXPECT_NE(info.unavailableReason.find("systemd-coredump"), std::string::npos);
}

} // namespace
} // namespace Platform
