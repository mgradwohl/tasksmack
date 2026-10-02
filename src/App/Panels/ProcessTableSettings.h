#pragma once

// Persisting the Processes table's column widths, order and sort between launches (#952).
//
// ImGui already knows how to save and restore a table's layout: it serialises it as a "[Table]"
// section of its ini text, with the font size the widths were measured at (RefScale) so they are
// rescaled when the font differs on restore. TaskSmack disables ImGui's ini *file* in favour of its
// own TOML config, and nothing replaced it for tables, so a resized or reordered column came back at
// its default on every launch. Rather than re-model widths and order by hand, the table's own
// section is carried in the TOML config as a string and handed back to ImGui at startup.
//
// The text comes back from a user-editable file and is fed to ImGui's settings parser, so it is not
// trusted. This header is the pure filter both directions go through; it is unit-testable without an
// ImGui context.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace App::ProcessTableSettings
{

/// Largest stored layout accepted. A real section is about 60 bytes a column, under 2 KiB in all;
/// anything far beyond that is not a table layout.
inline constexpr std::size_t MAX_STORED_BYTES = 8192;

/// Most lines accepted in a stored layout: a header, RefScale, and one line per column, with room
/// to spare for columns added in future versions.
inline constexpr std::size_t MAX_STORED_LINES = 128;

namespace Detail
{

[[nodiscard]] constexpr bool isHexDigit(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] constexpr bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

/// True for exactly "[Table][0xHHHHHHHH,N]" with N a positive decimal number.
[[nodiscard]] constexpr bool isTableHeader(std::string_view line) noexcept
{
    constexpr std::string_view PREFIX = "[Table][0x";
    constexpr std::size_t ID_DIGITS = 8;
    if (!line.starts_with(PREFIX) || line.size() < PREFIX.size() + ID_DIGITS + 3)
    {
        return false;
    }
    line.remove_prefix(PREFIX.size());
    for (std::size_t i = 0; i < ID_DIGITS; ++i)
    {
        if (!isHexDigit(line[i]))
        {
            return false;
        }
    }
    line.remove_prefix(ID_DIGITS);
    if (line.front() != ',' || line.back() != ']')
    {
        return false;
    }
    line.remove_prefix(1);
    line.remove_suffix(1);
    if (line.empty() || line.size() > 4)
    {
        return false;
    }
    return std::ranges::all_of(line, isDigit);
}

/// Characters that can appear in the "RefScale=" and "Column" lines ImGui writes: letters, digits,
/// spaces and the punctuation of "Width=60 Order=3 Sort=0^ ID=0x1A2B3C4D Weight=1.0000".
[[nodiscard]] constexpr bool isSettingsChar(char c) noexcept
{
    return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ' ' || c == '=' || c == '.' || c == '^' || c == '-' ||
           c == '+';
}

[[nodiscard]] constexpr bool isSettingsLine(std::string_view line) noexcept
{
    if (!line.starts_with("RefScale=") && !line.starts_with("Column "))
    {
        return false;
    }
    return std::ranges::all_of(line, isSettingsChar);
}

/// Appends `line` to `out` without any " Visible=N" token.
///
/// Column visibility already has a home in the config, [process_columns], which is hand-editable
/// and is what the Settings and context menus write. Leaving a second copy in the table layout
/// would let the two disagree, with ImGui's copy silently winning at startup.
inline void appendWithoutVisibility(std::string& out, std::string_view line)
{
    constexpr std::string_view TOKEN = " Visible=";
    for (;;)
    {
        const std::size_t at = line.find(TOKEN);
        if (at == std::string_view::npos)
        {
            out.append(line);
            return;
        }
        out.append(line.substr(0, at));
        line.remove_prefix(at + TOKEN.size());
        while (!line.empty() && isDigit(line.front()))
        {
            line.remove_prefix(1);
        }
    }
}

/// Removes and returns the first line of `text` (without its terminator).
[[nodiscard]] constexpr std::string_view takeLine(std::string_view& text) noexcept
{
    // By shrinking views rather than substr(), which is specified to throw on a bad position.
    const std::size_t newline = text.find('\n');
    std::string_view line = text;
    if (newline == std::string_view::npos)
    {
        text = {};
    }
    else
    {
        line.remove_suffix(line.size() - newline);
        text.remove_prefix(newline + 1);
    }
    if (!line.empty() && line.back() == '\r')
    {
        line.remove_suffix(1);
    }
    return line;
}

} // namespace Detail

/// Reduces stored text to one well-formed table section, or to nothing.
///
/// What survives is exactly: a "[Table][0x<id>,<columns>]" header, then "RefScale=" and "Column"
/// lines made only of the characters ImGui itself writes, with column visibility removed. Leading
/// blank lines are skipped; the section ends at the first blank line or the next header, so a
/// second section -- a window position, a docking layout, another table -- is never passed on. Text
/// that does not begin with a table header, or is implausibly large, yields an empty string.
[[nodiscard]] inline std::string sanitize(std::string_view stored)
{
    if (stored.size() > MAX_STORED_BYTES)
    {
        return {};
    }

    std::string_view line = Detail::takeLine(stored);
    while (line.empty() && !stored.empty())
    {
        line = Detail::takeLine(stored);
    }
    if (!Detail::isTableHeader(line))
    {
        return {};
    }

    std::string out;
    out.reserve(stored.size() + line.size() + 2);
    out.append(line);
    out.push_back('\n');

    std::size_t lines = 1;
    while (!stored.empty() && lines < MAX_STORED_LINES)
    {
        line = Detail::takeLine(stored);
        if (line.empty() || line.front() == '[')
        {
            break;
        }
        if (!Detail::isSettingsLine(line))
        {
            continue;
        }
        Detail::appendWithoutVisibility(out, line);
        out.push_back('\n');
        ++lines;
    }
    return out;
}

/// Pulls the section for one table out of ImGui's full ini text, sanitised.
///
/// @param ini      Text from ImGui::SaveIniSettingsToMemory().
/// @param tableId  The table's ImGuiID.
/// @return The table's section, or an empty string if it has none.
[[nodiscard]] inline std::string extractTableSection(std::string_view ini, std::uint32_t tableId)
{
    const std::string header = std::format("[Table][0x{:08X},", tableId);
    std::size_t at = 0;
    for (;;)
    {
        at = ini.find(header, at);
        if (at == std::string_view::npos)
        {
            return {};
        }
        if (at == 0 || ini[at - 1] == '\n')
        {
            break;
        }
        at += header.size();
    }
    // Hand sanitize() only this section: the rest of the ini can be large, and sanitize() rejects
    // oversized input outright.
    std::string_view section = ini.substr(at);
    if (const std::size_t next = section.find("\n[", 1); next != std::string_view::npos)
    {
        section = section.substr(0, next + 1);
    }
    return sanitize(section);
}

} // namespace App::ProcessTableSettings
