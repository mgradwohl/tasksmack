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

/// Most lines a stored section may have, counting the header and every line after it whether or
/// not it is kept: a header, RefScale, and one line per column, with room to spare for columns
/// added in future versions. A longer section is rejected whole, not truncated.
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

/// True if `text` is 1 to `maxDigits` decimal digits and nothing else. Bounding the digit count is
/// what keeps the value in range: ImGui reads these with "%d", and an integer too large for an int
/// is undefined behaviour there.
[[nodiscard]] constexpr bool isUnsigned(std::string_view text, std::size_t maxDigits) noexcept
{
    return !text.empty() && text.size() <= maxDigits && std::ranges::all_of(text, isDigit);
}

/// True if `text` is a plain decimal -- digits, optionally one '.' followed by digits -- with a
/// bounded number of digits either side. No sign, no exponent, and no letters, so "nan" and "inf"
/// (which "%f" accepts) cannot get through, and the value is finite by construction.
[[nodiscard]] constexpr bool isDecimal(std::string_view text, std::size_t maxIntegerDigits, std::size_t maxFractionDigits) noexcept
{
    const std::size_t dot = text.find('.');
    if (dot == std::string_view::npos)
    {
        return isUnsigned(text, maxIntegerDigits);
    }
    std::string_view fraction = text;
    fraction.remove_prefix(dot + 1);
    text.remove_suffix(text.size() - dot);
    return isUnsigned(text, maxIntegerDigits) && isUnsigned(fraction, maxFractionDigits);
}

/// True if `text` is exactly "0x" followed by eight hex digits.
[[nodiscard]] constexpr bool isHexId(std::string_view text) noexcept
{
    constexpr std::size_t ID_DIGITS = 8;
    if (!text.starts_with("0x") || text.size() != ID_DIGITS + 2)
    {
        return false;
    }
    text.remove_prefix(2);
    return std::ranges::all_of(text, isHexDigit);
}

/// True for exactly "[Table][0xHHHHHHHH,N]" with N a positive decimal number.
[[nodiscard]] constexpr bool isTableHeader(std::string_view line) noexcept
{
    constexpr std::string_view PREFIX = "[Table][";
    constexpr std::size_t ID_LENGTH = 10; // "0x" + eight hex digits
    if (!line.starts_with(PREFIX) || line.size() < PREFIX.size() + ID_LENGTH + 3 || line.back() != ']')
    {
        return false;
    }
    line.remove_prefix(PREFIX.size());
    line.remove_suffix(1);

    std::string_view columnCount = line;
    columnCount.remove_prefix(ID_LENGTH);
    line.remove_suffix(line.size() - ID_LENGTH);
    if (!isHexId(line) || columnCount.empty() || columnCount.front() != ',')
    {
        return false;
    }
    columnCount.remove_prefix(1);
    // Up to three digits: ImGui allows fewer than 512 columns.
    return isUnsigned(columnCount, 3);
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

/// Removes and returns the next space-separated token of `text`, skipping leading spaces.
[[nodiscard]] constexpr std::string_view takeToken(std::string_view& text) noexcept
{
    while (!text.empty() && text.front() == ' ')
    {
        text.remove_prefix(1);
    }
    const std::size_t space = text.find(' ');
    std::string_view token = text;
    if (space == std::string_view::npos)
    {
        text = {};
    }
    else
    {
        token.remove_suffix(token.size() - space);
        text.remove_prefix(space);
    }
    return token;
}

/// If `token` is "<key><value>", returns the value; otherwise an empty view.
[[nodiscard]] constexpr std::string_view valueOf(std::string_view token, std::string_view key) noexcept
{
    if (!token.starts_with(key))
    {
        return {};
    }
    token.remove_prefix(key.size());
    return token;
}

/// One "Column" line, reduced to the values that passed validation. Views point into the input.
struct ColumnLine
{
    std::string_view index;
    std::string_view width;  ///< Fixed columns: pixels.
    std::string_view weight; ///< Stretch columns: weight.
    std::string_view order;
    std::string_view sort; ///< Sort order followed by its direction, e.g. "0^".
    std::string_view id;
};

/// Parses a "Column N key=value ..." line against the exact grammar ImGui writes, returning false
/// if anything in it is not a known key with a well-formed, in-range value.
///
/// This is a grammar, not a character filter, because the result is read back by ImGui with
/// "%d" and "%f": a filter on characters alone lets through "Width=99999999999999999999" (an
/// out-of-range conversion, which is undefined behaviour) and "Weight=nan".
///
/// "Visible=" is recognised and deliberately dropped: [process_columns] owns visibility, and a
/// second copy here could disagree with it, with ImGui's copy silently winning at startup.
[[nodiscard]] constexpr bool parseColumnLine(std::string_view line, ColumnLine& out) noexcept
{
    constexpr std::string_view PREFIX = "Column ";
    if (!line.starts_with(PREFIX))
    {
        return false;
    }
    line.remove_prefix(PREFIX.size());

    out = ColumnLine{};
    out.index = takeToken(line);
    if (!isUnsigned(out.index, 3))
    {
        return false;
    }

    for (std::string_view token = takeToken(line); !token.empty(); token = takeToken(line))
    {
        if (const std::string_view value = valueOf(token, "Width="); !value.empty())
        {
            // Five digits: far beyond any real column, far inside an int.
            if (!isUnsigned(value, 5))
            {
                return false;
            }
            out.width = value;
        }
        else if (const std::string_view weightValue = valueOf(token, "Weight="); !weightValue.empty())
        {
            if (!isDecimal(weightValue, 4, 6))
            {
                return false;
            }
            out.weight = weightValue;
        }
        else if (const std::string_view visibleValue = valueOf(token, "Visible="); !visibleValue.empty())
        {
            if (!isUnsigned(visibleValue, 1))
            {
                return false;
            }
        }
        else if (const std::string_view orderValue = valueOf(token, "Order="); !orderValue.empty())
        {
            if (!isUnsigned(orderValue, 3))
            {
                return false;
            }
            out.order = orderValue;
        }
        else if (const std::string_view sortValue = valueOf(token, "Sort="); !sortValue.empty())
        {
            std::string_view digits = sortValue;
            digits.remove_suffix(1);
            const char direction = sortValue.back();
            if ((direction != 'v' && direction != '^') || !isUnsigned(digits, 2))
            {
                return false;
            }
            out.sort = sortValue;
        }
        else if (const std::string_view idValue = valueOf(token, "ID="); !idValue.empty())
        {
            if (!isHexId(idValue))
            {
                return false;
            }
            out.id = idValue;
        }
        else
        {
            return false;
        }
    }
    return true;
}

/// Writes a column line the way ImGui does, with its keys in the order ImGui's parser expects them
/// (it tries each key once, in sequence, so a line with its keys out of order is only partly read).
inline void appendColumnLine(std::string& out, const ColumnLine& column)
{
    out.append("Column ");
    out.append(column.index);
    if (column.index.size() < 2)
    {
        out.push_back(' ');
    }
    const auto appendField = [&out](std::string_view key, std::string_view value)
    {
        if (!value.empty())
        {
            out.push_back(' ');
            out.append(key);
            out.append(value);
        }
    };
    appendField("Width=", column.width);
    appendField("Weight=", column.weight);
    appendField("Order=", column.order);
    appendField("Sort=", column.sort);
    appendField("ID=", column.id);
    out.push_back('\n');
}

/// True for "RefScale=<decimal>": the font size the widths were measured at.
[[nodiscard]] constexpr bool isRefScaleLine(std::string_view line) noexcept
{
    return isDecimal(valueOf(line, "RefScale="), 4, 6);
}

} // namespace Detail

/// Reduces stored text to one well-formed table section, or to nothing.
///
/// What survives is exactly: a "[Table][0x<id>,<columns>]" header, then "RefScale=" and "Column"
/// lines that parse against the grammar ImGui itself writes -- known keys only, every number a
/// bounded run of digits, so nothing non-finite or out of range reaches ImGui's "%d"/"%f" parser --
/// rewritten in ImGui's own key order and with column visibility removed. Leading
/// blank lines are skipped; the section ends at the first blank line or the next header, so a
/// second section -- a window position, a docking layout, another table -- is never passed on. Text
/// that does not begin with a table header, is over MAX_STORED_BYTES, or whose section runs to more
/// than MAX_STORED_LINES lines (header included, kept or not) yields an empty string.
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

    // Every line of the section counts towards the limit, kept or not, and exceeding it rejects the
    // whole text rather than returning a truncated layout: a section that long is not something
    // ImGui wrote, and half of it is not a layout worth restoring.
    std::size_t lines = 1;
    while (!stored.empty())
    {
        line = Detail::takeLine(stored);
        if (line.empty() || line.front() == '[')
        {
            break;
        }
        if (++lines > MAX_STORED_LINES)
        {
            return {};
        }
        if (Detail::isRefScaleLine(line))
        {
            out.append(line);
            out.push_back('\n');
        }
        else if (Detail::ColumnLine column; Detail::parseColumnLine(line, column))
        {
            Detail::appendColumnLine(out, column);
        }
        // Anything else -- an unknown key, a malformed or out-of-range value -- is skipped.
    }
    return out;
}

/// Returns `captured` with the sort from `source` restored to any column that has lost it.
///
/// Tree View renders in parent/child order and drops ImGuiTableFlags_Sortable, and ImGui leaves the
/// sort out of a table's settings while it is not sortable. A layout captured in Tree View would
/// therefore overwrite the saved one with no sort at all, and the user's list-view sort would be
/// gone after a restart. `source` is the layout as it stood in list view; its sort is carried
/// across onto the freshly captured widths and order.
///
/// Only a capture with no sort at all is changed. If `captured` has a sort on any column, that sort
/// is newer than `source`'s and is returned as it is -- adding `source`'s on top would turn a
/// single-column sort into a two-column one. Both inputs are sanitised first, so the result is
/// always a well-formed section (or empty, if `captured` is not one).
[[nodiscard]] inline std::string carrySortForward(std::string_view captured, std::string_view source)
{
    std::string cleanCaptured = sanitize(captured); // not const: returned by move below
    const std::string cleanSource = sanitize(source);
    if (cleanCaptured.empty() || cleanSource.empty() || cleanCaptured.find(" Sort=") != std::string::npos)
    {
        return cleanCaptured;
    }

    std::string out;
    out.reserve(cleanCaptured.size() + 64);
    std::string_view remaining = cleanCaptured;
    while (!remaining.empty())
    {
        const std::string_view line = Detail::takeLine(remaining);
        Detail::ColumnLine column;
        if (!Detail::parseColumnLine(line, column))
        {
            out.append(line);
            out.push_back('\n');
            continue;
        }
        if (column.sort.empty())
        {
            std::string_view sourceRemaining = cleanSource;
            while (!sourceRemaining.empty())
            {
                Detail::ColumnLine sourceColumn;
                if (Detail::parseColumnLine(Detail::takeLine(sourceRemaining), sourceColumn) && sourceColumn.index == column.index)
                {
                    column.sort = sourceColumn.sort;
                    break;
                }
            }
        }
        Detail::appendColumnLine(out, column);
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
