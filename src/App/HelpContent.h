#pragma once

// What the Help window (#172) shows that needs no ImGui, so it is unit-tested directly
// (test_HelpContent.cpp): the shortcut filter, the one-line tab overview, and the column reference's
// platform notes. The window itself is App/HelpWindow.cpp.

#include "App/KeyboardShortcuts.h"
#include "App/ProcessColumnConfig.h"
#include "Platform/AsciiCase.h"
#include "Platform/AsciiTrim.h"

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
    {
        .name = "Machine (its name)",
        .summary = "The whole system: CPU, memory, storage, GPU, network and power, live and as history (Overview, CPU Cores, GPU, NPU "
                   "where there is one, Network and I/O)",
    },
    {.name = "Processes", .summary = "Every process, as a sortable list or a tree; filter it, pick columns, act on a selection"},
    {.name = "Process Details",
     .summary = "The selected process: Overview (identity, runtime, actions, charts), GPU, NPU where there is one, and Network and I/O"},
    {.name = "Services", .summary = "The system's services and their state (read-only)"},
}};

/// Which SHORTCUT_HELP entries a filter keeps, by index.
using ShortcutMask = std::array<bool, KeyboardShortcuts::SHORTCUT_HELP.size()>;

/// The shortcuts @p filter keeps: those whose keys, description or area name contain it, ignoring
/// case and surrounding blanks. A blank filter keeps every one. Allocation-free, so the window can
/// recompute it on each edit of the box.
[[nodiscard]] constexpr ShortcutMask matchingShortcuts(std::string_view filter) noexcept
{
    const std::string_view needle = Platform::trimAscii(filter);
    ShortcutMask mask{};
    std::ranges::transform(KeyboardShortcuts::SHORTCUT_HELP,
                           mask.begin(),
                           [needle](const KeyboardShortcuts::ShortcutHelpEntry& entry)
                           {
                               return Platform::containsIgnoreCase(entry.keys, needle) ||
                                      Platform::containsIgnoreCase(entry.description, needle) ||
                                      Platform::containsIgnoreCase(KeyboardShortcuts::areaLabel(entry.area), needle);
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
