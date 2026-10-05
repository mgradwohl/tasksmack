#pragma once

// Labels for the main tab bar whose ImGui ID does not change with their visible text (#1140).
// Pure string handling, no ImGui calls, so it is unit-testable directly (see CONTRIBUTING.md's
// "extract the pure decision logic into a small header" pattern).
//
// ImGui identifies a tab by a hash of its label. Text after "##" is hidden, and when the label holds
// "###" the hash restarts there, so only what follows the last "###" makes the ID. A label with a
// fixed "###" suffix can therefore change its visible text -- the Process Details tab shows the
// selected process's name -- while ImGui keeps treating it as the same tab. Without one, the tab's ID
// changed with the name, and ImGui, no longer finding the selected tab, selected another.

#include <cstddef>
#include <string>
#include <string_view>

namespace App::TabLabel
{

/// Starts the part of a label ImGui hashes as its ID and does not display.
inline constexpr std::string_view ID_SEPARATOR = "###";

/// The stable IDs of the main tabs.
inline constexpr std::string_view SYSTEM_TAB_ID = "SystemTab";
inline constexpr std::string_view PROCESSES_TAB_ID = "ProcessesTab";
inline constexpr std::string_view PROCESS_DETAILS_TAB_ID = "ProcessDetailsTab";

/// "<icon>  <text>###<stableId>": shows the icon and text, and is identified by `stableId` alone.
[[nodiscard]] inline std::string make(std::string_view icon, std::string_view text, std::string_view stableId)
{
    std::string label;
    label.reserve(icon.size() + 2 + text.size() + ID_SEPARATOR.size() + stableId.size());
    label.append(icon);
    label.append("  ");
    label.append(text);
    label.append(ID_SEPARATOR);
    label.append(stableId);
    return label;
}

/// The part of `label` ImGui's ID hash depends on: the text after its last "###", or the whole
/// label when it has none.
[[nodiscard]] constexpr std::string_view idPart(std::string_view label) noexcept
{
    const std::size_t separator = label.rfind(ID_SEPARATOR);
    if (separator != std::string_view::npos)
    {
        label.remove_prefix(separator + ID_SEPARATOR.size());
    }
    return label;
}

} // namespace App::TabLabel
