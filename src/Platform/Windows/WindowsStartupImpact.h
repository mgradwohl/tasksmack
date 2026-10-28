#pragma once

// The Startup tab's impact measurements on Windows (#3073), read only and without elevation.
//
// At each sign-in the Windows Diagnostic Infrastructure (WDI) traces the first 90 s or so of the
// session and records, per process that started, its CPU time and disk I/O, its parent, and when each
// started, in %SystemRoot%\System32\wdi\LogFiles\StartupInfo\<user SID>_StartupInfo<N>.xml (N = 1 to
// 5, a rotating set of the user's last five sign-ins, UTF-16). This is the data Task Manager's Startup
// impact is measured from. The folder can't be listed without administrator rights, but each of the
// user's own files can be opened by its full path, so TaskSmack reads it as the user, as Task Manager
// does. (The Diagnostics-Performance event log's events 100-110, which name only apps that slowed a
// boot and have no CPU or disk split, can't be read by a non-administrator at all: its channel's
// access list grants read to Administrators, SYSTEM, LocalService, NetworkService and service logons,
// not even to Event Log Readers.)
//
// The file reads go through an injectable table; WindowsStartupImpact.cpp supplies the real one, tests
// substitute fakes. The parser is standard library only (it shares WindowsCrashEvents.h's XML helpers),
// so its tests run on every platform.

#include "Platform/IStartupProbe.h"
#include "Platform/Windows/WindowsCrashEvents.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::WindowsStartupImpact
{

/// The StartupInfo files Windows keeps per user: <SID>_StartupInfo1.xml to _StartupInfo5.xml.
inline constexpr int FIRST_FILE = 1;
inline constexpr int LAST_FILE = 5;
/// A StartupInfo file is a few hundred KB; anything past this isn't one and isn't read.
inline constexpr std::size_t MAX_FILE_BYTES = std::size_t{16} * 1024U * 1024U;

inline constexpr std::uint32_t ERROR_FILE_NOT_FOUND_CODE = 2;
inline constexpr std::uint32_t ERROR_PATH_NOT_FOUND_CODE = 3;
inline constexpr std::uint32_t ERROR_ACCESS_DENIED_CODE = 5;

/// The calls readNewestSignIn() makes.
struct Functions
{
    /// The bytes of the current user's StartupInfo file number @p index (FIRST_FILE to LAST_FILE). False
    /// with the Win32 error in @p error when it can't be opened or read.
    bool (*readFile)(int index, std::vector<std::uint8_t>& bytes, std::uint32_t& error) = nullptr;
};

/// The real table, over the file system (WindowsStartupImpact.cpp; Windows only).
[[nodiscard]] Functions systemFunctions();

/// @p bytes as UTF-8 text: UTF-16LE when they start with its byte order mark (as Windows writes these
/// files) or look like it (a zero second byte), else UTF-8 with any byte order mark dropped. An
/// unpaired surrogate becomes U+FFFD; a trailing odd byte is dropped.
[[nodiscard]] inline std::string decodeText(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
    {
        return {bytes.begin() + 3, bytes.end()};
    }
    const bool bom = bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE;
    if (!bom && (bytes.size() < 2 || bytes[1] != 0))
    {
        return {bytes.begin(), bytes.end()};
    }
    std::string text;
    text.reserve(bytes.size() / 2);
    const auto append = [&text](std::uint32_t code)
    {
        if (code < 0x80U)
        {
            text += static_cast<char>(code);
        }
        else if (code < 0x800U)
        {
            text += static_cast<char>(0xC0U | (code >> 6U));
            text += static_cast<char>(0x80U | (code & 0x3FU));
        }
        else if (code < 0x10000U)
        {
            text += static_cast<char>(0xE0U | (code >> 12U));
            text += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
            text += static_cast<char>(0x80U | (code & 0x3FU));
        }
        else
        {
            text += static_cast<char>(0xF0U | (code >> 18U));
            text += static_cast<char>(0x80U | ((code >> 12U) & 0x3FU));
            text += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
            text += static_cast<char>(0x80U | (code & 0x3FU));
        }
    };
    constexpr std::uint32_t REPLACEMENT = 0xFFFD;
    const std::size_t units = bytes.size() / 2;
    const auto unitAt = [&bytes](std::size_t i)
    {
        return static_cast<std::uint32_t>(bytes[2 * i]) | (static_cast<std::uint32_t>(bytes[(2 * i) + 1]) << 8U);
    };
    for (std::size_t i = bom ? 1 : 0; i < units; ++i)
    {
        const std::uint32_t unit = unitAt(i);
        if (unit >= 0xD800U && unit <= 0xDBFFU && i + 1 < units)
        {
            const std::uint32_t low = unitAt(i + 1);
            if (low >= 0xDC00U && low <= 0xDFFFU)
            {
                append(0x10000U + ((unit - 0xD800U) << 10U) + (low - 0xDC00U));
                ++i;
                continue;
            }
        }
        append((unit >= 0xD800U && unit <= 0xDFFFU) ? REPLACEMENT : unit);
    }
    return text;
}

/// @p xml with each CDATA section's content removed, so a command line can never be taken for markup.
[[nodiscard]] inline std::string withoutCdata(std::string_view xml)
{
    constexpr std::string_view OPEN = "<![CDATA[";
    constexpr std::string_view CLOSE = "]]>";
    std::string out;
    out.reserve(xml.size());
    while (!xml.empty())
    {
        const std::size_t open = xml.find(OPEN);
        out += xml.substr(0, open);
        if (open == std::string_view::npos)
        {
            break;
        }
        const std::size_t close = xml.find(CLOSE, open + OPEN.size());
        if (close == std::string_view::npos)
        {
            break; // an unterminated section runs to the end
        }
        xml.remove_prefix(close + CLOSE.size());
    }
    return out;
}

/// A whole number; nullopt when @p text is empty or isn't one.
template<typename T> [[nodiscard]] std::optional<T> parseNumber(std::string_view text)
{
    T value{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

/// One of the file's times ("2026/10/23:16:52:27.9373641", UTC) as 100 ns ticks since 1970-01-01;
/// nullopt when malformed.
[[nodiscard]] inline std::optional<std::uint64_t> parseTime(std::string_view text)
{
    constexpr std::size_t SECONDS_LENGTH = 19; // "YYYY/MM/DD:HH:MM:SS"
    constexpr std::uint64_t TICKS_PER_SECOND = 10'000'000;
    constexpr std::size_t FRACTION_DIGITS = 7;
    if (text.size() < SECONDS_LENGTH || text[4] != '/' || text[7] != '/' || text[10] != ':')
    {
        return std::nullopt;
    }
    std::string iso(text.substr(0, SECONDS_LENGTH));
    iso[4] = '-';
    iso[7] = '-';
    iso[10] = 'T';
    const std::uint64_t seconds = WindowsCrashEvents::parseSystemTime(iso);
    if (seconds == 0)
    {
        return std::nullopt;
    }
    std::uint64_t ticks = seconds * TICKS_PER_SECOND;
    std::string_view fraction = text.substr(SECONDS_LENGTH);
    if (!fraction.empty())
    {
        if (fraction.front() != '.')
        {
            return std::nullopt;
        }
        fraction.remove_prefix(1);
        fraction = fraction.substr(0, FRACTION_DIGITS);
        const auto digits = parseNumber<std::uint64_t>(fraction);
        if (!digits.has_value())
        {
            return std::nullopt;
        }
        std::uint64_t scaled = *digits;
        for (std::size_t i = fraction.size(); i < FRACTION_DIGITS; ++i)
        {
            scaled *= 10;
        }
        ticks += scaled;
    }
    return ticks;
}

/// The decoded text of the first element named @p name inside [@p from, @p to) of @p xml; nullopt when
/// there is none.
[[nodiscard]] inline std::optional<std::string> childText(std::string_view xml, std::string_view name, std::size_t from, std::size_t to)
{
    const auto tag = WindowsCrashEvents::findTag(xml.substr(0, to), name, from);
    if (!tag.has_value())
    {
        return std::nullopt;
    }
    return WindowsCrashEvents::elementText(xml.substr(0, to), tag->first, tag->second);
}

/// One StartupInfo file's text (UTF-8) as a reading: its processes, ordered by start time, with when
/// the measurement began and how long it ran. nullopt when it isn't a StartupData document. A process
/// without a path, PID or start time is left out; a missing CPU or disk figure counts as 0.
[[nodiscard]] inline std::optional<StartupImpactReading> parseStartupInfoXml(std::string_view text)
{
    const std::string xml = withoutCdata(text);
    const auto root = WindowsCrashEvents::findTag(xml, "StartupData");
    if (!root.has_value())
    {
        return std::nullopt;
    }
    StartupImpactReading reading;
    const std::string_view rootTag = std::string_view(xml).substr(root->first, root->second - root->first);
    const auto intervalStart = parseNumber<std::uint64_t>(WindowsCrashEvents::tagAttribute(rootTag, "IntervalStartMs").value_or(""));
    const auto intervalEnd = parseNumber<std::uint64_t>(WindowsCrashEvents::tagAttribute(rootTag, "IntervalEndMs").value_or(""));
    if (intervalStart.has_value() && intervalEnd.has_value() && *intervalEnd > *intervalStart)
    {
        constexpr std::uint64_t MS_PER_SECOND = 1000;
        reading.windowSeconds = static_cast<std::uint32_t>(
            std::min<std::uint64_t>((*intervalEnd - *intervalStart) / MS_PER_SECOND, std::numeric_limits<std::uint32_t>::max()));
    }
    std::size_t from = root->second;
    while (const auto tag = WindowsCrashEvents::findTag(xml, "Process", from))
    {
        const std::size_t close = std::min(xml.find("</Process>", tag->second), xml.size());
        from = close;
        const std::string_view processTag = std::string_view(xml).substr(tag->first, tag->second - tag->first);
        StartupImpactProcess process;
        process.path = WindowsCrashEvents::tagAttribute(processTag, "Name").value_or("");
        const auto pid = parseNumber<std::uint32_t>(WindowsCrashEvents::tagAttribute(processTag, "PID").value_or(""));
        const auto start = parseTime(childText(xml, "StartTime", tag->second, close).value_or(""));
        if (process.path.empty() || !pid.has_value() || !start.has_value())
        {
            continue;
        }
        process.pid = *pid;
        process.startTicks = *start;
        process.parentPid = parseNumber<std::uint32_t>(childText(xml, "ParentPID", tag->second, close).value_or("")).value_or(0);
        process.parentStartTicks = parseTime(childText(xml, "ParentStartTime", tag->second, close).value_or("")).value_or(0);
        process.cpuUs = parseNumber<std::uint64_t>(childText(xml, "CpuUsage", tag->second, close).value_or("")).value_or(0);
        process.diskBytes = parseNumber<std::uint64_t>(childText(xml, "DiskUsage", tag->second, close).value_or("")).value_or(0);
        reading.processes.push_back(std::move(process));
    }
    std::ranges::stable_sort(reading.processes, {}, &StartupImpactProcess::startTicks);
    if (!reading.processes.empty())
    {
        constexpr std::uint64_t TICKS_PER_SECOND = 10'000'000;
        reading.signInUnixSeconds = reading.processes.front().startTicks / TICKS_PER_SECOND;
    }
    return reading;
}

/// Why no measurement could be read, from the files' errors: @p denied when any was refused, @p unreadable
/// when one was there but held no measurement, else none was found.
[[nodiscard]] inline std::string unavailableReason(bool denied, bool unreadable, std::uint32_t otherError)
{
    if (denied)
    {
        return "Windows didn't let TaskSmack read its sign-in measurements (access denied)";
    }
    if (otherError != 0)
    {
        return std::format("Windows' sign-in measurements couldn't be read (error {})", otherError);
    }
    if (unreadable)
    {
        return "Windows' sign-in measurements couldn't be read (not in the expected format)";
    }
    return "Windows hasn't measured a sign-in for this account yet";
}

/// The newest of the current user's sign-in measurements through @p fns: the file whose measurement
/// began last. Its unavailableReason says why when none could be read.
[[nodiscard]] inline StartupImpactReading readNewestSignIn(const Functions& fns)
{
    std::optional<StartupImpactReading> newest;
    bool denied = false;
    bool unreadable = false;
    std::uint32_t otherError = 0;
    for (int index = FIRST_FILE; index <= LAST_FILE; ++index)
    {
        std::vector<std::uint8_t> bytes;
        std::uint32_t error = 0;
        if (!fns.readFile(index, bytes, error))
        {
            if (error == ERROR_ACCESS_DENIED_CODE)
            {
                denied = true;
            }
            else if (error != ERROR_FILE_NOT_FOUND_CODE && error != ERROR_PATH_NOT_FOUND_CODE)
            {
                otherError = error;
            }
            continue;
        }
        std::optional<StartupImpactReading> reading = parseStartupInfoXml(decodeText(bytes));
        if (!reading.has_value())
        {
            unreadable = true;
            continue;
        }
        if (reading->processes.empty())
        {
            continue; // a measurement with nothing in it is no measurement
        }
        if (!newest.has_value() || reading->processes.front().startTicks > newest->processes.front().startTicks)
        {
            newest = std::move(reading);
        }
    }
    if (newest.has_value())
    {
        return *std::move(newest);
    }
    StartupImpactReading none;
    none.unavailableReason = unavailableReason(denied, unreadable, otherError);
    return none;
}

} // namespace Platform::WindowsStartupImpact
