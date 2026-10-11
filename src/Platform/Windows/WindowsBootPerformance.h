#pragma once

// The Windows Boot performance facts (#1525), read only: the newest event 100 of the
// Microsoft-Windows-Diagnostics-Performance/Operational log, which Windows writes once a boot has
// settled, with its BootTime, MainPathBootTime and BootPostBootTime (milliseconds). The log is readable
// by administrators only, so an unelevated TaskSmack says that rather than showing nothing.
// The wevtapi calls go through an injectable table; WindowsBootPerformance.cpp supplies the real one,
// tests substitute fakes. The parser is standard library only (it shares WindowsCrashEvents.h's XML
// helpers), so its tests run on every platform.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WindowsCrashEvents.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Platform::WindowsBootPerformance
{

inline constexpr std::string_view CHANNEL = "Microsoft-Windows-Diagnostics-Performance/Operational";
inline constexpr std::uint32_t BOOT_EVENT_ID = 100;
inline constexpr std::uint32_t ERROR_ACCESS_DENIED_CODE = 5;
inline constexpr std::uint32_t ERROR_NO_MORE_ITEMS_CODE = 259;
inline constexpr std::uint32_t ERROR_EVT_CHANNEL_NOT_FOUND_CODE = 15007;

/// The calls readBootPerformance() makes.
struct Functions
{
    /// The newest event matching @p xpath in CHANNEL, rendered as XML (UTF-8). False with the Win32 error in
    /// @p error when the log can't be queried or read, or ERROR_NO_MORE_ITEMS_CODE when no event matches.
    bool (*newestEventXml)(const std::string& xpath, std::string& xml, std::uint32_t& error) = nullptr;
};

/// The real table, over wevtapi (WindowsBootPerformance.cpp; Windows only).
[[nodiscard]] Functions systemFunctions();

/// The XPath filter for the boot event.
[[nodiscard]] inline std::string bootQueryXPath()
{
    return std::format("*[System[EventID={}]]", BOOT_EVENT_ID);
}

/// The timings one boot event carries.
struct BootEvent
{
    std::uint64_t unixSeconds = 0; ///< When it was logged (TimeCreated); 0 when unknown.
    std::optional<std::uint64_t> bootMs;
    std::optional<std::uint64_t> mainPathMs;
    std::optional<std::uint64_t> postBootMs;
};

/// The decoded text of the EventData item named @p name; nullopt when there is none.
[[nodiscard]] inline std::optional<std::string> eventDataValue(std::string_view xml, std::string_view name)
{
    const auto eventData = WindowsCrashEvents::findTag(xml, "EventData");
    if (!eventData)
    {
        return std::nullopt;
    }
    const std::size_t end = std::min(xml.find("</EventData>", eventData->second), xml.size());
    std::size_t from = eventData->second;
    while (const auto tag = WindowsCrashEvents::findTag(xml.substr(0, end), "Data", from))
    {
        if (WindowsCrashEvents::tagAttribute(xml.substr(tag->first, tag->second - tag->first), "Name") == name)
        {
            return WindowsCrashEvents::elementText(xml, tag->first, tag->second);
        }
        from = tag->second;
    }
    return std::nullopt;
}

/// A whole number of milliseconds; nullopt when absent or not a number.
[[nodiscard]] inline std::optional<std::uint64_t> parseMilliseconds(const std::optional<std::string>& text)
{
    if (!text.has_value() || text->empty())
    {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text->data(), text->data() + text->size(), value);
    if (ec != std::errc{} || end != text->data() + text->size())
    {
        return std::nullopt;
    }
    return value;
}

/// One Diagnostics-Performance event 100's XML as its timings; nullopt for any other event, or one with
/// no boot time in it.
[[nodiscard]] inline std::optional<BootEvent> parseBootEventXml(std::string_view xml)
{
    const auto idTag = WindowsCrashEvents::findTag(xml, "EventID");
    if (!idTag)
    {
        return std::nullopt;
    }
    const std::string idText = WindowsCrashEvents::elementText(xml, idTag->first, idTag->second);
    std::uint32_t id = 0;
    if (std::from_chars(idText.data(), idText.data() + idText.size(), id).ec != std::errc{} || id != BOOT_EVENT_ID)
    {
        return std::nullopt;
    }
    BootEvent event;
    event.mainPathMs = parseMilliseconds(eventDataValue(xml, "MainPathBootTime"));
    event.postBootMs = parseMilliseconds(eventDataValue(xml, "BootPostBootTime"));
    event.bootMs = parseMilliseconds(eventDataValue(xml, "BootTime"));
    if (!event.bootMs.has_value() && event.mainPathMs.has_value() && event.postBootMs.has_value())
    {
        event.bootMs = *event.mainPathMs + *event.postBootMs; // BootTime is their sum
    }
    if (!event.bootMs.has_value())
    {
        return std::nullopt;
    }
    if (const auto time = WindowsCrashEvents::findTag(xml, "TimeCreated"))
    {
        event.unixSeconds = WindowsCrashEvents::parseSystemTime(
            WindowsCrashEvents::tagAttribute(xml.substr(time->first, time->second - time->first), "SystemTime").value_or(""));
    }
    return event;
}

/// Why the boot event couldn't be read, from the Win32 error.
[[nodiscard]] inline std::string unavailableReason(std::uint32_t error)
{
    switch (error)
    {
    case ERROR_ACCESS_DENIED_CODE:
        return "Requires administrator (the Diagnostics-Performance event log is readable by administrators only)";
    case ERROR_NO_MORE_ITEMS_CODE:
        return "No boot has been recorded in the Diagnostics-Performance event log yet";
    case ERROR_EVT_CHANNEL_NOT_FOUND_CODE:
        return "The Diagnostics-Performance event log isn't available (missing or disabled)";
    default:
        return std::format("The Diagnostics-Performance event log couldn't be read (error {})", error);
    }
}

/// The newest boot's timings through @p fns.
inline void readBootPerformance(BootPerformanceInfo& info, const Functions& fns)
{
    constexpr std::uint64_t US_PER_MS = 1000;
    info.available = true;
    info.family = OsFamily::Windows;
    std::string xml;
    std::uint32_t error = 0;
    if (!fns.newestEventXml(bootQueryXPath(), xml, error))
    {
        info.unavailableReason = unavailableReason(error);
        return;
    }
    const std::optional<BootEvent> event = parseBootEventXml(xml);
    if (!event.has_value())
    {
        info.unavailableReason = "The last boot's event has no boot time in it";
        return;
    }
    const auto toUs = [](const std::optional<std::uint64_t>& ms)
    {
        return ms.has_value() ? std::optional<std::uint64_t>(*ms * US_PER_MS) : std::nullopt;
    };
    info.timingsRead = true;
    info.finished = true; // Windows logs the event only once the boot has settled
    info.totalUs = toUs(event->bootMs);
    info.mainPathUs = toUs(event->mainPathMs);
    info.postBootUs = toUs(event->postBootMs);
    info.recordedUnixSeconds = event->unixSeconds;
}

} // namespace Platform::WindowsBootPerformance
