#pragma once

// The Windows Recent crashes & hangs facts (#1524), read only: the Application event log's Application
// Error (1000) and Application Hang (1002) events of the last CRASH_HISTORY_DAYS days, newest first, from
// EvtQuery on the "Application" channel (readable without administrator), each rendered as event XML and
// parsed here. At most CRASH_LIST_MAX are kept. Nothing is cleared or written.
// Every wevtapi call goes through an injectable table; WindowsCrashEvents.cpp supplies the real one, tests
// substitute fakes. The parser is standard library only, so its fixture tests run on every platform.

#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::WindowsCrashEvents
{

inline constexpr std::uint32_t APPLICATION_ERROR_ID = 1000;
inline constexpr std::uint32_t APPLICATION_HANG_ID = 1002;
inline constexpr std::uint32_t ERROR_ACCESS_DENIED_CODE = 5;
inline constexpr std::uint32_t ERROR_EVT_CHANNEL_NOT_FOUND_CODE = 15007;

/// The calls readCrashes() makes. The query handle is opaque (an EVT_HANDLE).
struct Functions
{
    /// A newest-first query of the Application log; nullptr with the Win32 error in @p error on failure.
    void* (*openQuery)(const std::string& xpath, std::uint32_t& error) = nullptr;
    /// The next event as XML (UTF-8); false at the end or on a failure.
    bool (*nextEventXml)(void* query, std::string& xml) = nullptr;
    void (*closeQuery)(void* query) = nullptr;
};

/// The real table, over wevtapi (WindowsCrashEvents.cpp; Windows only).
[[nodiscard]] Functions systemFunctions();

/// The XPath filter: the two providers' events 1000 and 1002 of the last @p days.
[[nodiscard]] inline std::string crashQueryXPath(std::uint32_t days)
{
    constexpr std::uint64_t MS_PER_DAY = 86'400'000;
    return std::format("*[System[Provider[@Name='Application Error' or @Name='Application Hang'] and (EventID={} or EventID={}) and "
                       "TimeCreated[timediff(@SystemTime) <= {}]]]",
                       APPLICATION_ERROR_ID,
                       APPLICATION_HANG_ID,
                       static_cast<std::uint64_t>(days) * MS_PER_DAY);
}

/// XML text with its five named entities decoded ("&amp;" to "&"); any other reference is kept as it is.
[[nodiscard]] inline std::string unescapeXml(std::string_view text)
{
    static constexpr std::array<std::pair<std::string_view, char>, 5> NAMED{
        {
            {"&amp;", '&'},
            {"&lt;", '<'},
            {"&gt;", '>'},
            {"&quot;", '"'},
            {"&apos;", '\''},
        },
    };
    std::string out;
    out.reserve(text.size());
    while (!text.empty())
    {
        const std::size_t amp = text.find('&');
        out += text.substr(0, amp);
        if (amp == std::string_view::npos)
        {
            break;
        }
        text.remove_prefix(amp);
        std::size_t length = 1;
        char decoded = '&';
        for (const auto& [entity, character] : NAMED)
        {
            if (text.starts_with(entity))
            {
                length = entity.size();
                decoded = character;
            }
        }
        out += decoded;
        text.remove_prefix(length);
    }
    return out;
}

/// The start tag "<name ...>" or "<name .../>" at or after @p from: its offset and the offset past its '>'.
[[nodiscard]] inline std::optional<std::pair<std::size_t, std::size_t>>
findTag(std::string_view xml, std::string_view name, std::size_t from = 0)
{
    while (from < xml.size())
    {
        const std::size_t open = xml.find('<', from);
        if (open == std::string_view::npos || open + 1 + name.size() > xml.size())
        {
            return std::nullopt;
        }
        const std::size_t after = open + 1 + name.size();
        if (xml.substr(open + 1, name.size()) == name &&
            (after == xml.size() || xml[after] == ' ' || xml[after] == '>' || xml[after] == '/' || xml[after] == '\t' ||
             xml[after] == '\r' || xml[after] == '\n'))
        {
            const std::size_t close = xml.find('>', after);
            if (close == std::string_view::npos)
            {
                return std::nullopt;
            }
            return std::pair{open, close + 1};
        }
        from = open + 1;
    }
    return std::nullopt;
}

/// An attribute's decoded value in a start tag; nullopt when absent.
[[nodiscard]] inline std::optional<std::string> tagAttribute(std::string_view tag, std::string_view attribute)
{
    std::size_t at = 0;
    while ((at = tag.find(attribute, at)) != std::string_view::npos)
    {
        const bool boundary = at > 0 && (tag[at - 1] == ' ' || tag[at - 1] == '\t' || tag[at - 1] == '\r' || tag[at - 1] == '\n');
        std::size_t eq = at + attribute.size();
        while (eq < tag.size() && tag[eq] == ' ')
        {
            ++eq;
        }
        if (boundary && eq < tag.size() && tag[eq] == '=')
        {
            std::size_t quote = eq + 1;
            while (quote < tag.size() && tag[quote] == ' ')
            {
                ++quote;
            }
            if (quote < tag.size() && (tag[quote] == '\'' || tag[quote] == '"'))
            {
                const std::size_t end = tag.find(tag[quote], quote + 1);
                if (end != std::string_view::npos)
                {
                    return unescapeXml(tag.substr(quote + 1, end - quote - 1));
                }
            }
            return std::nullopt;
        }
        at += attribute.size();
    }
    return std::nullopt;
}

/// The decoded text of the element whose start tag ends at @p contentStart, up to its end tag.
[[nodiscard]] inline std::string elementText(std::string_view xml, std::size_t tagStart, std::size_t contentStart)
{
    if (contentStart >= 2 && xml[contentStart - 2] == '/' && contentStart - 2 >= tagStart) // "<Data Name='x'/>"
    {
        return {};
    }
    const std::size_t end = xml.find('<', contentStart);
    return unescapeXml(xml.substr(contentStart, end == std::string_view::npos ? std::string_view::npos : end - contentStart));
}

/// An ISO 8601 UTC SystemTime ("2026-10-01T14:03:22.1234567Z") as Unix seconds; 0 when malformed.
[[nodiscard]] inline std::uint64_t parseSystemTime(std::string_view text)
{
    const auto number = [&](std::size_t at, std::size_t width) -> std::optional<int>
    {
        int value = 0;
        if (at + width > text.size())
        {
            return std::nullopt;
        }
        const auto [end, ec] = std::from_chars(text.data() + at, text.data() + at + width, value);
        return ec == std::errc{} && end == text.data() + at + width ? std::optional<int>(value) : std::nullopt;
    };
    constexpr std::size_t LENGTH = 19; // "YYYY-MM-DDTHH:MM:SS"
    if (text.size() < LENGTH || text[4] != '-' || text[7] != '-' || (text[10] != 'T' && text[10] != ' ') || text[13] != ':' ||
        text[16] != ':')
    {
        return 0;
    }
    const auto year = number(0, 4);
    const auto month = number(5, 2);
    const auto day = number(8, 2);
    const auto hour = number(11, 2);
    const auto minute = number(14, 2);
    const auto second = number(17, 2);
    if (!year || !month || !day || !hour || !minute || !second)
    {
        return 0;
    }
    const std::chrono::year_month_day date{
        std::chrono::year{*year}, std::chrono::month{static_cast<unsigned>(*month)}, std::chrono::day{static_cast<unsigned>(*day)}};
    constexpr int HOURS = 23;
    constexpr int MINUTES = 59;
    constexpr int SECONDS = 60; // a leap second
    if (!date.ok() || *hour > HOURS || *minute > MINUTES || *second > SECONDS)
    {
        return 0;
    }
    const auto seconds = std::chrono::sys_days{date}.time_since_epoch() + std::chrono::hours{*hour} + std::chrono::minutes{*minute} +
                         std::chrono::seconds{*second};
    const auto count = std::chrono::duration_cast<std::chrono::seconds>(seconds).count();
    return count > 0 ? static_cast<std::uint64_t>(count) : 0;
}

/// An exception code as "0xC0000005", from "c0000005" or "0xc0000005"; the text as it is when not hex.
[[nodiscard]] inline std::string formatExceptionCode(std::string_view text)
{
    std::string_view digits = text;
    if (digits.starts_with("0x") || digits.starts_with("0X"))
    {
        digits.remove_prefix(2);
    }
    constexpr std::size_t MAX_DIGITS = 8;
    if (digits.empty() || digits.size() > MAX_DIGITS || !std::ranges::all_of(digits, [](unsigned char c) { return std::isxdigit(c) != 0; }))
    {
        return std::string(text);
    }
    std::string out = "0x";
    out.append(MAX_DIGITS - digits.size(), '0');
    for (const char c : digits)
    {
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

/// One Application Error (1000) or Application Hang (1002) event's XML as a crash; nullopt for any other
/// event or text that isn't one. 1000 names its data ("AppName", "ModuleName", "ExceptionCode", ...); an
/// older 1000 and 1002 list them in order, so both are read.
[[nodiscard]] inline std::optional<CrashEvent> parseEventXml(std::string_view xml)
{
    const auto idTag = findTag(xml, "EventID");
    if (!idTag)
    {
        return std::nullopt;
    }
    const std::string idText = elementText(xml, idTag->first, idTag->second);
    std::uint32_t id = 0;
    if (std::from_chars(idText.data(), idText.data() + idText.size(), id).ec != std::errc{} ||
        (id != APPLICATION_ERROR_ID && id != APPLICATION_HANG_ID))
    {
        return std::nullopt;
    }
    if (const auto provider = findTag(xml, "Provider"))
    {
        const std::optional<std::string> name = tagAttribute(xml.substr(provider->first, provider->second - provider->first), "Name");
        if (name.has_value() && *name != (id == APPLICATION_ERROR_ID ? "Application Error" : "Application Hang"))
        {
            return std::nullopt;
        }
    }
    CrashEvent event;
    event.hang = id == APPLICATION_HANG_ID;
    if (const auto time = findTag(xml, "TimeCreated"))
    {
        event.unixSeconds = parseSystemTime(tagAttribute(xml.substr(time->first, time->second - time->first), "SystemTime").value_or(""));
    }
    // The EventData values, by name where named, else in order.
    std::vector<std::pair<std::string, std::string>> data;
    if (const auto eventData = findTag(xml, "EventData"))
    {
        const std::size_t end = std::min(xml.find("</EventData>", eventData->second), xml.size());
        std::size_t from = eventData->second;
        while (const auto tag = findTag(xml.substr(0, end), "Data", from))
        {
            data.emplace_back(tagAttribute(xml.substr(tag->first, tag->second - tag->first), "Name").value_or(""),
                              elementText(xml, tag->first, tag->second));
            from = tag->second;
        }
    }
    const bool named = std::ranges::any_of(data, [](const auto& item) { return !item.first.empty(); });
    const auto value = [&](std::string_view name, std::size_t index) -> std::string
    {
        if (named)
        {
            const auto it = std::ranges::find(data, name, &std::pair<std::string, std::string>::first);
            return it == data.end() ? std::string{} : it->second;
        }
        return index < data.size() ? data[index].second : std::string{};
    };
    event.application = value("AppName", 0);
    event.appVersion = value("AppVersion", 1);
    std::string pid;
    if (event.hang)
    {
        constexpr std::size_t HANG_TYPE = 9;
        pid = value("ProcessId", 2);
        event.hangType = value("HangType", HANG_TYPE);
    }
    else
    {
        constexpr std::size_t MODULE = 3;
        constexpr std::size_t MODULE_VERSION = 4;
        constexpr std::size_t EXCEPTION = 6;
        constexpr std::size_t PROCESS = 8;
        event.module = value("ModuleName", MODULE);
        event.moduleVersion = value("ModuleVersion", MODULE_VERSION);
        if (event.moduleVersion == "0.0.0.0") // an "unknown" module
        {
            event.moduleVersion.clear();
        }
        const std::string code = value("ExceptionCode", EXCEPTION);
        event.exceptionCode = code.empty() ? std::string{} : formatExceptionCode(code);
        pid = value("ProcessId", PROCESS);
    }
    // Both write the process id in hex, with or without "0x".
    std::string_view digits = pid;
    if (digits.starts_with("0x") || digits.starts_with("0X"))
    {
        digits.remove_prefix(2);
    }
    constexpr int HEX = 16;
    if (std::uint32_t number = 0; !digits.empty())
    {
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), number, HEX);
        if (ec == std::errc{} && end == digits.data() + digits.size())
        {
            event.pid = number;
        }
    }
    return event;
}

/// The crashes and hangs through @p fns, newest first; at most @p maxEvents.
inline void readCrashes(CrashesInfo& info, const Functions& fns, std::size_t maxEvents = CRASH_LIST_MAX)
{
    info.available = true;
    info.family = OsFamily::Windows;
    std::uint32_t error = 0;
    void* query = fns.openQuery(crashQueryXPath(CRASH_HISTORY_DAYS), error);
    if (query == nullptr)
    {
        info.accessDenied = error == ERROR_ACCESS_DENIED_CODE;
        if (info.accessDenied)
        {
            info.unavailableReason = "Reading the Application event log requires permission";
        }
        else if (error == ERROR_EVT_CHANNEL_NOT_FOUND_CODE)
        {
            info.unavailableReason = "The Application event log isn't available (missing or disabled)";
        }
        else
        {
            info.unavailableReason = std::format("The Application event log couldn't be read (error {})", error);
        }
        return;
    }
    info.listed = true;
    std::string xml;
    while (fns.nextEventXml(query, xml))
    {
        std::optional<CrashEvent> event = parseEventXml(xml);
        if (!event.has_value())
        {
            continue;
        }
        if (info.events.size() == maxEvents)
        {
            info.capped = true;
            break;
        }
        info.events.push_back(std::move(*event));
    }
    fns.closeQuery(query);
    // The query reads newest first; keep that order even if the log's clock went backwards.
    std::ranges::stable_sort(info.events, std::ranges::greater{}, &CrashEvent::unixSeconds);
}

} // namespace Platform::WindowsCrashEvents
