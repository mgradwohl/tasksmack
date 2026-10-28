/// @file test_WindowsStartupImpact.cpp
/// @brief The Windows Startup impact reader (#3073), through a fake file table: the WDI StartupInfo
/// parser on a captured sign-in (StartupInfoTestData.h), its UTF-16 decoding, times, CDATA and
/// entities, and the choice of the newest of the five files and the reasons when none can be read.
/// Standard library only, so it runs everywhere; no real file is read.

#include "Platform/IStartupProbe.h"
#include "Platform/StartupImpact.h"
#include "Platform/Windows/WindowsStartupImpact.h"
#include "StartupInfoTestData.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{
namespace
{

using namespace WindowsStartupImpact;

constexpr std::uint64_t TICKS_PER_SECOND = 10'000'000;
constexpr std::uint64_t SIGN_IN_UNIX_SECONDS = 1792774334;       // 2026-10-23 16:52:14 UTC
constexpr std::uint64_t OLDER_SIGN_IN_UNIX_SECONDS = 1792483862; // 2026-10-20 08:11:02 UTC

/// @p text (ASCII or UTF-8 of the BMP) as UTF-16LE bytes, with a byte order mark when @p bom.
[[nodiscard]] std::vector<std::uint8_t> utf16le(std::u16string_view text, bool bom = true)
{
    std::vector<std::uint8_t> bytes;
    if (bom)
    {
        bytes = {0xFF, 0xFE};
    }
    for (const char16_t unit : text)
    {
        bytes.push_back(static_cast<std::uint8_t>(unit & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>(unit >> 8U));
    }
    return bytes;
}

/// @p ascii widened unit for unit (the test data is ASCII).
[[nodiscard]] std::u16string widen(std::string_view ascii)
{
    return {ascii.begin(), ascii.end()};
}

// The fake file table: each of the five files' bytes, or the error opening it gives.
struct FakeFile
{
    std::vector<std::uint8_t> bytes;
    std::uint32_t error = ERROR_FILE_NOT_FOUND_CODE;
};
std::array<FakeFile, LAST_FILE> g_Files; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::vector<int> g_Read;                 // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

[[nodiscard]] Functions fakeFiles()
{
    g_Files = {};
    g_Read.clear();
    return {
        .readFile = [](int index, std::vector<std::uint8_t>& bytes, std::uint32_t& error) -> bool
        {
            g_Read.push_back(index);
            const FakeFile& file = g_Files.at(static_cast<std::size_t>(index - FIRST_FILE));
            if (file.error != 0)
            {
                error = file.error;
                return false;
            }
            bytes = file.bytes;
            return true;
        },
    };
}

void setFile(int index, std::string_view xml)
{
    g_Files.at(static_cast<std::size_t>(index - FIRST_FILE)) = {.bytes = utf16le(widen(xml)), .error = 0};
}

void setError(int index, std::uint32_t error)
{
    g_Files.at(static_cast<std::size_t>(index - FIRST_FILE)) = {.bytes = {}, .error = error};
}

TEST(WindowsStartupImpactTest, ParsesEveryProcessOfASignIn)
{
    const auto readingParsed = parseStartupInfoXml(StartupInfoTestData::SIGN_IN_XML);
    ASSERT_TRUE(readingParsed.has_value());
    const StartupImpactReading reading = readingParsed.value_or(StartupImpactReading{});
    EXPECT_TRUE(reading.unavailableReason.empty());
    EXPECT_EQ(reading.windowSeconds, 90U);
    EXPECT_EQ(reading.signInUnixSeconds, SIGN_IN_UNIX_SECONDS);
    ASSERT_EQ(reading.processes.size(), 9U);

    // Ordered by start time: OneDrive started first.
    const StartupImpactProcess& oneDrive = reading.processes[0];
    EXPECT_EQ(oneDrive.path, R"(C:\Users\Sam\AppData\Local\Microsoft\OneDrive\OneDrive.exe)");
    EXPECT_EQ(oneDrive.pid, 9120U);
    EXPECT_EQ(oneDrive.cpuUs, 13239718U);
    EXPECT_EQ(oneDrive.diskBytes, 100827136U);
    EXPECT_EQ(oneDrive.parentPid, 10276U);
    EXPECT_EQ(oneDrive.startTicks, (SIGN_IN_UNIX_SECONDS * TICKS_PER_SECOND) + 1180331U);
    EXPECT_EQ(oneDrive.parentStartTicks, ((SIGN_IN_UNIX_SECONDS - 1) * TICKS_PER_SECOND) + 8738290U);
    for (std::size_t i = 1; i < reading.processes.size(); ++i)
    {
        EXPECT_LE(reading.processes[i - 1].startTicks, reading.processes[i].startTicks) << i;
    }
    // The entity in the last one's path is decoded.
    EXPECT_EQ(reading.processes.back().path, R"(C:\Program Files (x86)\Contoso & Co\Updater\updater.exe)");
}

TEST(WindowsStartupImpactTest, ACommandLinesCdataIsNeverReadAsMarkup)
{
    // The Edge renderer's command line holds "</Process><CpuUsage ...>999999999</CpuUsage>".
    const auto readingParsed = parseStartupInfoXml(StartupInfoTestData::SIGN_IN_XML);
    ASSERT_TRUE(readingParsed.has_value());
    const StartupImpactReading reading = readingParsed.value_or(StartupImpactReading{});
    const auto renderer = std::ranges::find(reading.processes, 11188U, &StartupImpactProcess::pid);
    ASSERT_NE(renderer, reading.processes.end());
    EXPECT_EQ(renderer->cpuUs, 4100000U);
    EXPECT_EQ(renderer->parentPid, 11032U);
    for (const auto& process : reading.processes)
    {
        EXPECT_NE(process.cpuUs, 999999999U);
    }
    EXPECT_EQ(withoutCdata("a<![CDATA[<b>]]>c<![CDATA[d"), "ac");
}

TEST(WindowsStartupImpactTest, EdgesTreeIsMeasuredWithItsChildrenAndNotThePidThatReusedOne)
{
    const auto readingParsed = parseStartupInfoXml(StartupInfoTestData::SIGN_IN_XML);
    ASSERT_TRUE(readingParsed.has_value());
    const StartupImpactReading reading = readingParsed.value_or(StartupImpactReading{});
    const StartupImpactTotals edge = measureStartupProgram(reading, R"("C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe")");
    EXPECT_EQ(edge.processCount, 4U); // itself, its renderer and WebView2, and WebView2's conhost
    EXPECT_EQ(edge.cpuUs, 214700U + 4100000U + 1800000U + 90000U);
    EXPECT_EQ(edge.diskBytes, 359051264U + 52428800U + 40000000U + 8192U);

    const StartupImpactTotals updater = measureStartupProgram(reading, R"(C:\Program Files (x86)\Contoso & Co\Updater\updater.exe)");
    EXPECT_EQ(updater.processCount, 1U);
    EXPECT_EQ(updater.cpuUs, 3969U);
}

TEST(WindowsStartupImpactTest, ProcessesWithoutWhatIdentifiesThemAreLeftOut)
{
    constexpr std::string_view XML = R"(<StartupData IntervalStartMs="9" IntervalEndMs="5">
<Process Name="" PID="1"><StartTime>2026/10/23:16:52:14.0</StartTime></Process>
<Process Name="C:\a.exe" PID="x"><StartTime>2026/10/23:16:52:14.0</StartTime></Process>
<Process Name="C:\b.exe" PID="2"><StartTime>yesterday</StartTime></Process>
<Process Name="C:\c.exe" PID="3"><StartTime>2026/10/23:16:52:14.5</StartTime><CpuUsage Units="us">many</CpuUsage></Process>
</StartupData>)";
    const auto readingParsed = parseStartupInfoXml(XML);
    ASSERT_TRUE(readingParsed.has_value());
    const StartupImpactReading reading = readingParsed.value_or(StartupImpactReading{});
    EXPECT_EQ(reading.windowSeconds, 0U) << "an interval that ends before it starts is no interval";
    ASSERT_EQ(reading.processes.size(), 1U);
    EXPECT_EQ(reading.processes[0].path, R"(C:\c.exe)");
    EXPECT_EQ(reading.processes[0].cpuUs, 0U) << "a figure that isn't a number counts as 0";
    EXPECT_EQ(reading.processes[0].parentPid, 0U);
    EXPECT_EQ(reading.processes[0].startTicks % TICKS_PER_SECOND, 5000000U);

    EXPECT_FALSE(parseStartupInfoXml("<Event><System/></Event>").has_value());
    const auto emptyParsed = parseStartupInfoXml("<StartupData/>");
    ASSERT_TRUE(emptyParsed.has_value());
    const StartupImpactReading empty = emptyParsed.value_or(StartupImpactReading{});
    EXPECT_TRUE(empty.processes.empty());
    EXPECT_EQ(empty.signInUnixSeconds, 0U);
}

TEST(WindowsStartupImpactTest, ParsesTheFilesTimes)
{
    EXPECT_EQ(parseTime("2026/10/23:16:52:14"), SIGN_IN_UNIX_SECONDS * TICKS_PER_SECOND);
    EXPECT_EQ(parseTime("2026/10/23:16:52:14.1"), (SIGN_IN_UNIX_SECONDS * TICKS_PER_SECOND) + 1000000U);
    EXPECT_EQ(parseTime("2026/10/23:16:52:14.123456789"), (SIGN_IN_UNIX_SECONDS * TICKS_PER_SECOND) + 1234567U) << "past 100 ns: cut";
    EXPECT_FALSE(parseTime("").has_value());
    EXPECT_FALSE(parseTime("2026-10-23T16:52:14.0").has_value()) << "not the file's own form";
    EXPECT_FALSE(parseTime("2026/13/23:16:52:14").has_value());
    EXPECT_FALSE(parseTime("2026/10/23:16:52:14,5").has_value());
    EXPECT_FALSE(parseTime("2026/10/23:16:52:14.x").has_value());
    EXPECT_FALSE(parseTime("2026/10/23:16:52:14.").has_value());
}

TEST(WindowsStartupImpactTest, DecodesUtf16AndUtf8)
{
    // UTF-16LE with and without a byte order mark, as Windows writes the files.
    EXPECT_EQ(decodeText(utf16le(u"<a>", true)), "<a>");
    EXPECT_EQ(decodeText(utf16le(u"<a>", false)), "<a>");
    // U+00E9, U+20AC, U+1F600 (a surrogate pair) and an unpaired surrogate, which becomes U+FFFD.
    const std::u16string mixed = {u'\u00E9', u'\u20AC', 0xD83D, 0xDE00, 0xDC00, u'x', 0xD800};
    EXPECT_EQ(decodeText(utf16le(mixed)), "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xEF\xBF\xBDx\xEF\xBF\xBD");
    // A trailing odd byte is dropped.
    std::vector<std::uint8_t> odd = utf16le(u"ab");
    odd.push_back(0x41);
    EXPECT_EQ(decodeText(odd), "ab");
    // UTF-8, with and without its byte order mark, is kept as it is.
    const std::vector<std::uint8_t> utf8 = {0xEF, 0xBB, 0xBF, '<', 'b', '>'};
    EXPECT_EQ(decodeText(utf8), "<b>");
    const std::vector<std::uint8_t> plain = {'<', 'c', '>'};
    EXPECT_EQ(decodeText(plain), "<c>");
    EXPECT_EQ(decodeText({}), "");
}

TEST(WindowsStartupImpactTest, ReadsTheNewestOfTheFiveFiles)
{
    const Functions fns = fakeFiles();
    setFile(2, StartupInfoTestData::OLDER_SIGN_IN_XML);
    setFile(4, StartupInfoTestData::SIGN_IN_XML);
    setFile(5, StartupInfoTestData::OLDER_SIGN_IN_XML);
    const StartupImpactReading reading = readNewestSignIn(fns);
    EXPECT_EQ(g_Read, (std::vector<int>{1, 2, 3, 4, 5}));
    EXPECT_TRUE(reading.unavailableReason.empty());
    EXPECT_EQ(reading.signInUnixSeconds, SIGN_IN_UNIX_SECONDS);
    EXPECT_EQ(reading.processes.size(), 9U);

    // With only the older one there, it is read.
    setError(4, ERROR_FILE_NOT_FOUND_CODE);
    const StartupImpactReading older = readNewestSignIn(fns);
    EXPECT_EQ(older.signInUnixSeconds, OLDER_SIGN_IN_UNIX_SECONDS);
    ASSERT_EQ(older.processes.size(), 1U);
    EXPECT_EQ(older.processes[0].cpuUs, 1000U);
}

TEST(WindowsStartupImpactTest, SaysWhyWhenNoFileCanBeRead)
{
    const Functions fns = fakeFiles();
    EXPECT_EQ(readNewestSignIn(fns).unavailableReason, "Windows hasn't measured a sign-in for this account yet");

    setError(3, ERROR_PATH_NOT_FOUND_CODE);
    setFile(4, R"(<StartupData IntervalStartMs="1" IntervalEndMs="2"></StartupData>)"); // nothing measured
    EXPECT_EQ(readNewestSignIn(fns).unavailableReason, "Windows hasn't measured a sign-in for this account yet");

    setFile(1, "not XML at all");
    EXPECT_EQ(readNewestSignIn(fns).unavailableReason, "Windows' sign-in measurements couldn't be read (not in the expected format)");

    constexpr std::uint32_t ERROR_SHARING_VIOLATION_CODE = 32;
    setError(2, ERROR_SHARING_VIOLATION_CODE);
    EXPECT_EQ(readNewestSignIn(fns).unavailableReason, "Windows' sign-in measurements couldn't be read (error 32)");

    setError(5, ERROR_ACCESS_DENIED_CODE);
    const StartupImpactReading denied = readNewestSignIn(fns);
    EXPECT_EQ(denied.unavailableReason, "Windows didn't let TaskSmack read its sign-in measurements (access denied)");
    EXPECT_TRUE(denied.processes.empty());

    // One readable file is enough, whatever the others say.
    setFile(2, StartupInfoTestData::SIGN_IN_XML);
    EXPECT_TRUE(readNewestSignIn(fns).unavailableReason.empty());
}

} // namespace
} // namespace Platform
