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
//
// The visible text is external -- a process name, a hostname -- and may hold "#"s of its own. Put in
// the label as is, a "##" in it hid the rest of the text (#1244), and a "#" at its end ran into the
// label's "###" suffix and moved the point where ImGui restarts its hash, changing the ID.
// appendDisplayText() therefore follows such "#"s with a zero-width space, which the bundled Inter
// font maps to an empty, zero-advance glyph: the text looks the same, but holds no "##".

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace App::TabLabel
{

/// Starts the part of a label ImGui hashes as its ID and does not display.
inline constexpr std::string_view ID_SEPARATOR = "###";

/// U+200B ZERO WIDTH SPACE in UTF-8: keeps two "#"s from reading as ImGui's "##".
inline constexpr std::string_view HASH_BREAK = "\xE2\x80\x8B";

/// The stable IDs of the main tabs.
inline constexpr std::string_view SYSTEM_TAB_ID = "SystemTab";
inline constexpr std::string_view PROCESSES_TAB_ID = "ProcessesTab";
inline constexpr std::string_view PROCESS_DETAILS_TAB_ID = "ProcessDetailsTab";

/// The stable ID of the Process Details panel's own window (ProcessDetailsPanel::render()).
inline constexpr std::string_view PROCESS_DETAILS_WINDOW_ID = "ProcessDetails";

/// Appends `text` to `label` so that ImGui shows all of it: a HASH_BREAK follows every "#" that is
/// followed by another "#" or ends the text, so the text holds no "##" and forms none with what the
/// caller appends next (such as the "###" suffix).
inline void appendDisplayText(std::string& label, std::string_view text)
{
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        label.push_back(text[i]);
        if (text[i] == '#' && (i + 1 == text.size() || text[i + 1] == '#'))
        {
            label.append(HASH_BREAK);
        }
    }
}

/// "<icon>  <text>###<stableId>": shows the icon and all of `text`, and is identified by `stableId`
/// alone, whatever "#"s `text` holds (see appendDisplayText()).
[[nodiscard]] inline std::string make(std::string_view icon, std::string_view text, std::string_view stableId)
{
    std::string label;
    label.reserve(icon.size() + 2 + text.size() + ID_SEPARATOR.size() + stableId.size());
    label.append(icon);
    label.append("  ");
    appendDisplayText(label, text);
    label.append(ID_SEPARATOR);
    label.append(stableId);
    return label;
}

/// The title of the Process Details panel's own window: `processName`, or "Process Details" when there
/// is no process to name. Its ID is PROCESS_DETAILS_WINDOW_ID whatever the name, so ImGui keeps the
/// window's position and size when the selection changes.
[[nodiscard]] inline std::string makeProcessDetailsWindowLabel(std::string_view icon, std::string_view processName)
{
    return make(icon, processName.empty() ? std::string_view{"Process Details"} : processName, PROCESS_DETAILS_WINDOW_ID);
}

/// The part of `label` ImGui's ID hash depends on. Mirrors ImHashStr() in the pinned ImGui (1.92.9b):
/// scanning left to right, a "#" followed by "##" resets the CRC to the seed and the scan resumes after
/// all three, so the "###" itself is not hashed (ImGui commit fc89c61; older versions reset without
/// skipping it). The ID is what follows the last restart, or the whole label when there is none. That
/// is not always the text after the last "###": in "a####b" the restart is at the first "#", and the
/// ID is "#b". test_TabLabel.cpp checks this against ImHashStr() itself.
[[nodiscard]] constexpr std::string_view idPart(std::string_view label) noexcept
{
    std::size_t start = 0;
    std::size_t i = 0;
    while (i < label.size())
    {
        if (label[i] == '#' && i + 2 < label.size() && label[i + 1] == '#' && label[i + 2] == '#')
        {
            i += ID_SEPARATOR.size();
            start = i;
        }
        else
        {
            ++i;
        }
    }
    label.remove_prefix(start);
    return label;
}

/// The part of `label` ImGui displays: everything before its first "##" (ImGui::FindRenderedTextEnd()).
[[nodiscard]] constexpr std::string_view visiblePart(std::string_view label) noexcept
{
    if (const std::size_t end = label.find("##"); end != std::string_view::npos)
    {
        label.remove_suffix(label.size() - end);
    }
    return label;
}

/// A label rebuilt only when the text it shows changes, so a caller can hand ImGui the same label
/// every frame without allocating (#1326).
class CachedLabel
{
  public:
    /// The label for `text`: `build(text)` when `text` differs from the text the cached label was built
    /// from, or when nothing has been built yet; otherwise the cached label. The new label and key are
    /// built first and committed together last, so if `build` throws the cache keeps its previous,
    /// matching label and key, and the next call tries again.
    template<typename Build> const std::string& get(std::string_view text, Build&& build)
    {
        if (!m_Valid || text != m_Text)
        {
            std::string label = std::forward<Build>(build)(text);
            std::string key{text};
            m_Label = std::move(label);
            m_Text = std::move(key);
            m_Valid = true;
        }
        return m_Label;
    }

    /// The label the last get() returned (empty before the first).
    [[nodiscard]] const std::string& label() const noexcept
    {
        return m_Label;
    }

  private:
    std::string m_Label;
    std::string m_Text;
    bool m_Valid = false;
};

} // namespace App::TabLabel
