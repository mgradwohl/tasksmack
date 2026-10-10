#pragma once

// What the Help window (#172) shows that needs no ImGui, so it is unit-tested directly
// (test_HelpContent.cpp): the shortcut filter, the one-line tab overview, and the column reference's
// platform notes. The window itself is App/HelpWindow.cpp.

#include "App/KeyboardShortcuts.h"
#include "App/ProcessColumnConfig.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace App::HelpContent
{

inline constexpr const char* USER_GUIDE_URL = "https://github.com/mgradwohl/tasksmack/blob/main/docs/guide/user-guide.md";
inline constexpr const char* ISSUES_URL = "https://github.com/mgradwohl/tasksmack/issues";
// The two links' text, shared by the Help window and About (#1600), which both show them.
inline constexpr const char* USER_GUIDE_LINK_LABEL = "User guide (online)";
inline constexpr const char* ISSUES_LINK_LABEL = "Report a problem or ask for a feature";

/// One line of the tab overview.
struct TabSummary
{
    std::string_view name;
    std::string_view summary;
};

/// One line for each top-level tab, in the tab bar's order (ShellLayer's tab list).
inline constexpr std::array<TabSummary, 4> TAB_OVERVIEW{{
    {.name = "Machine (its name)",
     .summary = "The whole system: CPU, memory, storage, GPU, network and power, live and as history (Overview, CPU Cores, GPU, "
                "Network and I/O)"},
    {.name = "Processes", .summary = "Every process, as a sortable list or a tree; filter it, pick columns, act on a selection"},
    {.name = "Process Details", .summary = "The selected process: Overview (identity, runtime, actions, charts), GPU, and Network and I/O"},
    {.name = "Services", .summary = "The system's services and their state (read-only)"},
}};

/// Which SHORTCUT_HELP entries a filter keeps, by index.
using ShortcutMask = std::array<bool, KeyboardShortcuts::SHORTCUT_HELP.size()>;

namespace Detail
{

[[nodiscard]] constexpr char lowerAscii(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Whether @p text contains @p needle, ignoring ASCII case. An empty needle is in everything.
[[nodiscard]] constexpr bool containsIgnoreCase(std::string_view text, std::string_view needle) noexcept
{
    // std::ranges::search, not substr(): nothing here can throw.
    return needle.empty() || !std::ranges::search(text, needle, [](char a, char b) { return lowerAscii(a) == lowerAscii(b); }).empty();
}

[[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace Detail

/// The shortcuts @p filter keeps: those whose keys, description or area name contain it, ignoring
/// case and surrounding blanks. A blank filter keeps every one. Allocation-free, so the window can
/// recompute it on each edit of the box.
[[nodiscard]] constexpr ShortcutMask matchingShortcuts(std::string_view filter) noexcept
{
    const std::string_view needle = Detail::trim(filter);
    ShortcutMask mask{};
    std::ranges::transform(KeyboardShortcuts::SHORTCUT_HELP,
                           mask.begin(),
                           [needle](const KeyboardShortcuts::ShortcutHelpEntry& entry)
                           {
                               return Detail::containsIgnoreCase(entry.keys, needle) ||
                                      Detail::containsIgnoreCase(entry.description, needle) ||
                                      Detail::containsIgnoreCase(KeyboardShortcuts::areaLabel(entry.area), needle);
                           });
    return mask;
}

/// How many entries @p mask keeps.
[[nodiscard]] constexpr std::size_t countMatches(const ShortcutMask& mask) noexcept
{
    return static_cast<std::size_t>(std::ranges::count(mask, true));
}

/// The label for the platform the column's capability note (columnCapabilityNote()) applies to, or
/// empty when the column has none. The Processes header's tooltip shows that note only where the
/// probe lacks the counter; the reference names the platform instead, so it reads the same on every
/// machine. Windows has no user-mode per-process UDP byte counters (#1258).
[[nodiscard]] constexpr std::string_view columnNotePlatform(ProcessColumn col) noexcept
{
    return columnCapabilityNote(col, /*hasUdpNetworkCounters=*/false).empty() ? std::string_view{} : std::string_view{"On Windows:"};
}

} // namespace App::HelpContent
