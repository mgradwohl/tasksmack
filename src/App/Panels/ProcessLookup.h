#pragma once

// The Processes row menu's "Open file location" and "Search online" (#3064): what they open and the
// result line each leaves. Pure, so the tests read them without a file manager or a browser.

#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "Platform/IProcessActions.h"
#include "Platform/UriEncoding.h"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace App::Detail
{

/// Where "Search online" sends the process name: DuckDuckGo, which keeps no search history.
inline constexpr std::string_view SEARCH_ONLINE_URL_PREFIX = "https://duckduckgo.com/?q=";

/// The web search "Search online" opens for process @p name: "<name> process", percent-encoded. The
/// name only: never the executable's path or the command line, which can hold user names and folders.
[[nodiscard]] inline std::string searchOnlineUrl(std::string_view name)
{
    return std::string(SEARCH_ONLINE_URL_PREFIX) + Platform::percentEncode(std::format("{} process", name));
}

/// "Opened the file location of bash (PID 321)", with the platform's note when it has one, or "Could not
/// open the file location of bash (PID 321): ...".
[[nodiscard]] inline ActionResultMessage
formatOpenFileLocationResult(std::string_view name, std::int32_t pid, const Platform::ProcessActionResult& result)
{
    if (!result.success)
    {
        return {.ok = false, .text = std::format("Could not open the file location of {} (PID {}): {}", name, pid, result.errorMessage)};
    }
    std::string text = std::format("Opened the file location of {} (PID {})", name, pid);
    if (!result.successNote.empty())
    {
        text += std::format(" ({})", result.successNote);
    }
    return {.ok = true, .text = std::move(text)};
}

/// "Searching online for bash" or "Could not search online for bash: ...".
[[nodiscard]] inline ActionResultMessage formatSearchOnlineResult(std::string_view name, const Platform::ProcessActionResult& result)
{
    if (!result.success)
    {
        return {.ok = false, .text = std::format("Could not search online for {}: {}", name, result.errorMessage)};
    }
    return {.ok = true, .text = std::format("Searching online for {}", name)};
}

} // namespace App::Detail
