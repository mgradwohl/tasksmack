#pragma once

// One trim for every layer (#2170). There were seven, each stripping its own set: isspace() in three
// (locale-dependent), ' ' and '\t' in HelpContent, ' ' '\t' '\r' in DesktopEntryIcons, " \t\r\n" in
// LinuxCpuDetails and NVMLGPUProbeMath. So a value with a trailing '\r' (copied from a Windows file)
// was accepted by one TASKSMACK_* hook and rejected by another. Platform is the lowest layer, so
// Domain, Core, UI and App code can all include it, as they already include Platform/ThreadName.h.

#include <string_view>

namespace Platform
{

/// What trimAscii() strips: the C locale's isspace() set, without isspace()'s locale dependence.
inline constexpr std::string_view ASCII_WHITESPACE = " \t\n\v\f\r";

/// @p text without leading and trailing ASCII whitespace (ASCII_WHITESPACE). A view into @p text.
[[nodiscard]] constexpr std::string_view trimAscii(std::string_view text) noexcept
{
    const std::size_t first = text.find_first_not_of(ASCII_WHITESPACE);
    if (first == std::string_view::npos)
    {
        return {};
    }
    // remove_prefix/remove_suffix, not substr(), which can throw: this is noexcept.
    text.remove_suffix(text.size() - text.find_last_not_of(ASCII_WHITESPACE) - 1);
    text.remove_prefix(first);
    return text;
}

} // namespace Platform
