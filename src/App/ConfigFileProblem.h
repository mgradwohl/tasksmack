#pragma once

// Why config.toml couldn't be used (#4318), in words for the user. A config file that can't be opened or parsed is
// kept as it is (#1122): TaskSmack runs on its defaults and saves nothing over it, since a save would drop every
// setting and unknown key in it. That used to show only in tasksmack-debug.log, so every change made in the session
// was silently lost. UserConfig records the reason (UserConfig::fileProblem()); the status bar shows a warning with
// it, and Settings says it beside Edit Config File. Header-only and pure, so it is tested on every host.

#include "Domain/Utf8Sanitize.h"

#include <toml++/toml.hpp>

#include <format>
#include <string>
#include <string_view>

namespace App::ConfigFileProblem
{

/// What a problem notice ends with: what TaskSmack does about it, and what the user can do.
inline constexpr std::string_view CONSEQUENCE = "TaskSmack is using its default settings and won't save over the file "
                                                "until it's fixed: open it with Edit Config File in Settings.";

/// Whether @p bytes look like UTF-16 text (a UTF-16 byte-order mark, or a NUL byte, which UTF-8 TOML never holds):
/// what Notepad's "Unicode" and Windows PowerShell 5.1's Out-File write.
[[nodiscard]] constexpr bool looksLikeUtf16(std::string_view bytes) noexcept
{
    if (bytes.size() >= 2 && ((bytes[0] == '\xFF' && bytes[1] == '\xFE') || (bytes[0] == '\xFE' && bytes[1] == '\xFF')))
    {
        return true;
    }
    return bytes.contains('\0');
}

/// The notice for a config file at @p pathText (display text) that couldn't be opened.
[[nodiscard]] inline std::string couldNotOpen(std::string_view pathText)
{
    return Domain::sanitizedUtf8(std::format("{} couldn't be opened. {}", pathText, CONSEQUENCE));
}

/// The notice for a save refused at @p pathText (display text) for the reason @p why, a phrase that follows the path
/// ("is a link, which TaskSmack run as root doesn't follow") (#4653). The file loaded, or there was none, so its settings
/// are the ones in use: what's lost is only this session's changes, which the notice says, never "default settings".
[[nodiscard]] inline std::string couldNotSave(std::string_view pathText, std::string_view why)
{
    return Domain::sanitizedUtf8(std::format("This session's settings changes aren't being saved: {} {}.", pathText, why));
}

/// The notice for a config file at @p pathText (display text) whose @p bytes toml++ refused with @p error: the line,
/// column and reason, or, for UTF-16 text, how to save it so TaskSmack can read it.
[[nodiscard]] inline std::string couldNotParse(std::string_view pathText, std::string_view bytes, const toml::parse_error& error)
{
    if (looksLikeUtf16(bytes))
    {
        return Domain::sanitizedUtf8(std::format("{} is saved as UTF-16; save it as UTF-8. {}", pathText, CONSEQUENCE));
    }
    const toml::source_position where = error.source().begin;
    return Domain::sanitizedUtf8(std::format(
        "{} couldn't be read: line {}, column {}: {}. {}", pathText, where.line, where.column, error.description(), CONSEQUENCE));
}

} // namespace App::ConfigFileProblem
