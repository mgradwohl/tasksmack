#include "StartupView.h"

#include "App/Panels/ProcessIconView.h"
#include "App/Panels/ProcessTableLayout.h"
#include "App/Panels/StartupActionsView.h"
#include "Domain/ProcessIconCache.h"
#include "Domain/StartupImpact.h"
#include "Domain/StartupModel.h"
#include "Platform/AsciiCase.h"
#include "Platform/IStartupProbe.h"
#include "UI/CellText.h"
#include "UI/EmptyState.h"
#include "UI/Format.h"
#include "UI/FrameOutline.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>
// imgui_stdlib.h extends imgui.h's API for std::string, so it follows it.
#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <misc/cpp/imgui_stdlib.h>

namespace App
{

namespace
{

constexpr const char* FILTER_HINT = ICON_FA_MAGNIFYING_GLASS "  Filter startup apps...";

using Platform::compareIgnoreCase;
using Platform::containsIgnoreCase;

/// An entry's impact, or Not measured when @p impacts doesn't reach it.
[[nodiscard]] Domain::StartupEntryImpact impactAt(std::span<const Domain::StartupEntryImpact> impacts, std::size_t index) noexcept
{
    return index < impacts.size() ? impacts[index] : Domain::StartupEntryImpact{};
}

/// Startup impact's order (#3073): the rating, then CPU time, then disk I/O.
[[nodiscard]] std::strong_ordering compareImpact(const Domain::StartupEntryImpact& a, const Domain::StartupEntryImpact& b) noexcept
{
    if (const std::strong_ordering order = a.rating <=> b.rating; order != 0)
    {
        return order;
    }
    if (const std::strong_ordering order = a.cpuUs <=> b.cpuUs; order != 0)
    {
        return order;
    }
    return a.diskBytes <=> b.diskBytes;
}

[[nodiscard]] std::strong_ordering compareBy(const Platform::StartupEntry& a, const Platform::StartupEntry& b, StartupColumn column)
{
    switch (column)
    {
    case StartupColumn::Publisher:
        return compareIgnoreCase(a.publisher, b.publisher);
    case StartupColumn::Enabled:
        // Enabled first; the names within each group are ordered by buildStartupRows (#1597).
        if (a.enabled != b.enabled)
        {
            return a.enabled ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        return std::strong_ordering::equal;
    case StartupColumn::Scope:
        return a.scope <=> b.scope;
    case StartupColumn::Location:
        return a.location <=> b.location;
    case StartupColumn::Command:
        return compareIgnoreCase(a.command, b.command);
    case StartupColumn::Impact: // by buildStartupRows, which has the impacts
    case StartupColumn::Name:
    default:
        return compareIgnoreCase(a.name, b.name);
    }
}

/// The Enabled column's order within a group (#1597): by name, case-insensitively, then exactly, then
/// by where the entry is registered, so two entries never compare equal by name alone.
[[nodiscard]] std::strong_ordering compareByName(const Platform::StartupEntry& a, const Platform::StartupEntry& b)
{
    if (const std::strong_ordering order = compareIgnoreCase(a.name, b.name); order != 0)
    {
        return order;
    }
    if (const std::strong_ordering order = a.name <=> b.name; order != 0)
    {
        return order;
    }
    if (const std::strong_ordering order = a.location <=> b.location; order != 0)
    {
        return order;
    }
    // One program started by two values in one key shows one name twice (#1873).
    if (const std::strong_ordering order = startupEntryKey(a) <=> startupEntryKey(b); order != 0)
    {
        return order;
    }
    return a.packageFamily <=> b.packageFamily;
}

/// The entry's source, command and resolved program, and why it is flagged when its program is missing.
void renderStartupTooltip(const Platform::StartupEntry& entry)
{
    const auto& scheme = UI::Theme::get().scheme();
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0F);
    ImGui::TextUnformatted(entry.name.c_str());
    if (const std::string registration = startupRegistrationLabel(entry); !registration.empty())
    {
        ImGui::TextColored(scheme.textMuted, "%s", registration.c_str());
    }
    if (const std::string program = startupProgramNameLabel(entry); !program.empty())
    {
        ImGui::TextColored(scheme.textMuted, "%s", program.c_str());
    }
    if (entry.target == Platform::StartupTargetState::Missing)
    {
        ImGui::TextColored(scheme.textWarning, ICON_FA_TRIANGLE_EXCLAMATION "  The program this entry starts was not found");
    }
    else if (entry.target == Platform::StartupTargetState::Unresolved && !entry.sourcePath.empty() && entry.command.empty())
    {
        ImGui::TextColored(scheme.textMuted, "Shortcut target unresolved");
    }
    if (!entry.command.empty())
    {
        ImGui::Text("Command: %s", entry.command.c_str());
    }
    if (!entry.executablePath.empty())
    {
        ImGui::Text("Program: %s", entry.executablePath.c_str());
    }
    if (!entry.sourcePath.empty())
    {
        ImGui::Text("Shortcut: %s", entry.sourcePath.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

/// Rebuilds the rows only when what they are built from changed: at most once per sample, sort or
/// filter edit, never every frame (#580). The Enabled column's text is rebuilt with a new sample.
void rebuildRowsIfStale(const Domain::StartupPublication& publication, StartupViewState& state)
{
    const bool newSample = state.rowsVersion != publication.version || state.enabledLabels.size() != publication.entries.size();
    if (!newSample && state.rowsFilter == state.filter && state.rowsColumn == state.sortColumn && state.rowsAscending == state.ascending)
    {
        return;
    }
    if (newSample)
    {
        state.enabledLabels.clear();
        state.enabledLabels.reserve(publication.entries.size());
        state.iconKeys.clear();
        state.iconKeys.reserve(publication.entries.size());
        for (const auto& entry : publication.entries)
        {
            state.enabledLabels.push_back(startupEnabledLabel(entry));
            state.iconKeys.push_back(Domain::iconFileKey(startupIconPath(entry)));
        }
    }
    state.rows = buildStartupRows(publication.entries, state.filter, state.sortColumn, state.ascending, publication.impacts);
    state.rowsVersion = publication.version;
    state.rowsFilter = state.filter;
    state.rowsColumn = state.sortColumn;
    state.rowsAscending = state.ascending;
}

} // namespace

std::string_view startupLocationLabel(Platform::StartupLocation location) noexcept
{
    using enum Platform::StartupLocation;
    switch (location)
    {
    case RunUser:
        return "Registry: HKCU Run";
    case RunMachine:
        return "Registry: HKLM Run";
    case RunMachine32:
        return "Registry: HKLM Run (32-bit)";
    case RunOnceUser:
        return "Registry: HKCU RunOnce";
    case RunOnceMachine:
        return "Registry: HKLM RunOnce";
    case StartupFolderUser:
        return "Startup folder";
    case StartupFolderCommon:
        return "Startup folder (all users)";
    case PackagedApp:
        return "Packaged app";
    case XdgAutostartUser:
        return "Autostart (your folder)";
    case XdgAutostartSystem:
        return "Autostart (system)";
    default:
        return "";
    }
}

std::string_view startupImpactLabel(Domain::StartupImpact impact) noexcept
{
    using enum Domain::StartupImpact;
    switch (impact)
    {
    case None:
        return "None";
    case Low:
        return "Low";
    case Medium:
        return "Medium";
    case High:
        return "High";
    case NotMeasured:
    default:
        return "Not measured";
    }
}

std::string
startupImpactTooltip(const Domain::StartupEntryImpact& impact, std::uint64_t signInUnixSeconds, std::string_view unavailableReason)
{
    if (impact.rating == Domain::StartupImpact::None)
    {
        return "Disabled: it doesn't start when you sign in.";
    }
    const std::string signIn = UI::Format::formatEpochDateTime(signInUnixSeconds);
    if (impact.rating == Domain::StartupImpact::NotMeasured)
    {
        if (!unavailableReason.empty())
        {
            return "Not measured: " + std::string(unavailableReason) + ".";
        }
        if (signIn.empty())
        {
            return "Not measured: Windows has no figures for its program at the last sign-in.";
        }
        return "Not measured: its program didn't run during the measurement of the sign-in at " + signIn +
               " (or TaskSmack couldn't tell which program it starts).";
    }
    constexpr double US_PER_MS = 1000.0;
    std::string text = signIn.empty() ? std::string("At the last sign-in:") : "At the sign-in at " + signIn + ":";
    text += "\nCPU time: " + UI::Format::formatFixedLocalized(static_cast<double>(impact.cpuUs) / US_PER_MS, 0, " ms");
    text += "\nDisk I/O: " + UI::Format::formatBytes(static_cast<double>(impact.diskBytes));
    if (impact.processCount > 1)
    {
        text += "\n" + std::to_string(impact.processCount) + " processes: the program and those it started";
    }
    text += "\n\nHigh: over 1 s of CPU or 3 MB of disk I/O. Low: under 300 ms of CPU and 300 KB of disk I/O. "
            "Medium: in between. Task Manager's thresholds.";
    return text;
}

std::string_view startupScopeLabel(Platform::StartupScope scope) noexcept
{
    return scope == Platform::StartupScope::Machine ? "All users" : "Current user";
}

std::string_view startupEntryKey(const Platform::StartupEntry& entry) noexcept
{
    return entry.registrationName.empty() ? std::string_view(entry.name) : std::string_view(entry.registrationName);
}

std::string startupRegistrationLabel(const Platform::StartupEntry& entry)
{
    using enum Platform::StartupLocation;
    switch (entry.location)
    {
    case RunUser:
    case RunMachine:
    case RunMachine32:
    case RunOnceUser:
    case RunOnceMachine:
        if (!entry.registrationName.empty() && entry.registrationName != entry.name)
        {
            return "Registry value: " + entry.registrationName;
        }
        return {};
    case PackagedApp:
        if (!entry.packageFamily.empty() && !entry.registrationName.empty())
        {
            return "Startup task: " + entry.packageFamily + "!" + entry.registrationName;
        }
        return {};
    case StartupFolderUser:
    case StartupFolderCommon:
    default:
        return {};
    }
}

std::string startupProgramNameLabel(const Platform::StartupEntry& entry)
{
    if (entry.programName.empty() || entry.programName == entry.name)
    {
        return {};
    }
    return "Program name: " + entry.programName;
}

std::string startupEnabledLabel(const Platform::StartupEntry& entry)
{
    if (entry.setByPolicy)
    {
        return entry.enabled ? "Enabled by policy" : "Disabled by policy";
    }
    if (!entry.notSwitchableReason.empty() && !entry.enabled)
    {
        return "Not started here"; // its tooltip and the actions say why
    }
    if (entry.enabled)
    {
        return "Enabled";
    }
    const std::string when = UI::Format::formatEpochDateTime(entry.disabledAtUnixSeconds);
    // "YYYY-MM-DD HH:MM:SS" in local time: the column shows the date.
    return when.empty() ? std::string("Disabled") : "Disabled since " + when.substr(0, when.find(' '));
}

std::string_view startupIconPath(const Platform::StartupEntry& entry) noexcept
{
    if (!entry.iconPath.empty())
    {
        return entry.iconPath;
    }
    if (entry.target == Platform::StartupTargetState::Missing)
    {
        return {}; // Nothing to read an icon from
    }
    return entry.executablePath;
}

std::vector<std::size_t> buildStartupRows(std::span<const Platform::StartupEntry> entries,
                                          std::string_view filter,
                                          StartupColumn column,
                                          bool ascending,
                                          std::span<const Domain::StartupEntryImpact> impacts)
{
    std::vector<std::size_t> rows;
    rows.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const auto& entry = entries[i];
        if (containsIgnoreCase(entry.name, filter) || containsIgnoreCase(entry.registrationName, filter) ||
            containsIgnoreCase(entry.programName, filter) || containsIgnoreCase(entry.publisher, filter) ||
            containsIgnoreCase(entry.command, filter))
        {
            rows.push_back(i);
        }
    }
    std::ranges::stable_sort(rows,
                             [&](std::size_t a, std::size_t b)
                             {
                                 const std::strong_ordering order = (column == StartupColumn::Impact)
                                                                      ? compareImpact(impactAt(impacts, a), impactAt(impacts, b))
                                                                      : compareBy(entries[a], entries[b], column);
                                 if (order != 0)
                                 {
                                     return ascending ? (order < 0) : (order > 0);
                                 }
                                 // Within enabled or disabled the names stay A to Z in either direction
                                 // (#1597); only the order of the two groups reverses.
                                 return column == StartupColumn::Enabled && compareByName(entries[a], entries[b]) < 0;
                             });
    return rows;
}

namespace
{

/// The empty state when the platform has no startup list: the probe's reason as the heading, built
/// once (it is fixed when the probe is made), as the Services tab does.
void renderUnsupported(const Platform::StartupCapabilities& capabilities, StartupViewState& state)
{
    if (state.unavailableHeading.empty())
    {
        state.unavailableHeading =
            std::string(ICON_FA_POWER_OFF "  ") +
            (capabilities.unavailableReason.empty() ? std::string("Startup apps aren't available") : capabilities.unavailableReason);
    }
    UI::Widgets::renderEmptyState(state.unavailableHeading.c_str(),
                                  "TaskSmack can list startup apps on Windows. Support for XDG autostart on Linux is planned.");
}

/// The filter box and the "N of M startup apps" count beside it. The rows are brought up to date with
/// the filter before the count is drawn, so the count matches the table below.
void renderFilterBar(const Domain::StartupPublication& publication, StartupViewState& state, const UI::ColorScheme& scheme)
{
    ImGui::SetNextItemWidth(ProcessTableLayout::computeFilterWidth(
        ImGui::CalcTextSize(FILTER_HINT).x, ImGui::GetStyle().FramePadding.x, ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, scheme.textMuted);
    {
        const UI::Widgets::FrameOutlineScope frameOutline;
        ImGui::InputTextWithHint("##StartupFilter", FILTER_HINT, &state.filter);
    }
    ImGui::PopStyleColor();
    rebuildRowsIfStale(publication, state);
    ImGui::SameLine();
    ImGui::TextColored(scheme.textMuted, "%zu of %zu startup apps", state.rows.size(), publication.entries.size());
}

/// The table's columns, in StartupColumn order, each tagged with its StartupColumn as the user ID the
/// sort specs report; the header row is frozen while scrolling.
void setupStartupColumns(const Platform::StartupCapabilities& capabilities)
{
    const float em = ImGui::GetFontSize();
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_None, em * 14.0F, static_cast<ImGuiID>(StartupColumn::Name));
    ImGui::TableSetupColumn("Publisher", ImGuiTableColumnFlags_None, em * 12.0F, static_cast<ImGuiID>(StartupColumn::Publisher));
    ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_DefaultSort, em * 11.0F, static_cast<ImGuiID>(StartupColumn::Enabled));
    // Highest first on the first click. Where the platform has no impact data (Linux) the column is
    // left out altogether, not offered in the header menu.
    const ImGuiTableColumnFlags impactFlags =
        ImGuiTableColumnFlags_PreferSortDescending | (capabilities.hasImpact ? ImGuiTableColumnFlags_None : ImGuiTableColumnFlags_Disabled);
    ImGui::TableSetupColumn("Startup impact", impactFlags, em * 8.0F, static_cast<ImGuiID>(StartupColumn::Impact));
    ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_None, em * 6.0F, static_cast<ImGuiID>(StartupColumn::Scope));
    ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_None, em * 12.0F, static_cast<ImGuiID>(StartupColumn::Location));
    ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_None, em * 30.0F, static_cast<ImGuiID>(StartupColumn::Command));
    ImGui::TableHeadersRow();
}

/// Takes a header click into the view state; the rows are rebuilt from it afterwards.
void applySortSpecs(StartupViewState& state)
{
    ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
    if (specs == nullptr || !specs->SpecsDirty)
    {
        return;
    }
    if (specs->SpecsCount > 0)
    {
        state.sortColumn = static_cast<StartupColumn>(specs->Specs[0].ColumnUserID);
        state.ascending = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
    }
    specs->SpecsDirty = false;
}

[[nodiscard]] bool isSelected(const Platform::StartupEntry& entry, const StartupViewState& state)
{
    return entry.location == state.selectedLocation && startupEntryKey(entry) == state.selectedName &&
           entry.packageFamily == state.selectedFamily;
}

/// What a row draws besides its entry: the action menu, and the icon (#1800) filed under iconKey.
struct RowExtras
{
    StartupActionsView* actions = nullptr;
    ProcessIconView* icons = nullptr;
    /// The entry's Startup impact (#3073) and the publication it is from, for its tooltip; null where
    /// the platform has no impact data.
    const Domain::StartupEntryImpact* impact = nullptr;
    const Domain::StartupPublication* publication = nullptr;
    std::uint64_t iconKey = 0;
};

/// The Startup impact cell (#3073): the rating, muted when there is none to give.
[[nodiscard]] bool renderImpactCell(const Domain::StartupEntryImpact& impact, const UI::ColorScheme& scheme)
{
    const bool rated = impact.rating != Domain::StartupImpact::None && impact.rating != Domain::StartupImpact::NotMeasured;
    ImGui::PushStyleColor(ImGuiCol_Text, rated ? scheme.textPrimary : scheme.textMuted);
    const bool tooltip = UI::Widgets::cellText(startupImpactLabel(impact.rating)).tooltipShown;
    ImGui::PopStyleColor();
    return tooltip;
}

/// The Startup impact cell's tooltip, in place of the entry's while the pointer is over that cell.
void renderImpactTooltip(const Domain::StartupEntryImpact& impact, const Domain::StartupPublication& publication)
{
    const std::string text = startupImpactTooltip(impact, publication.impactSignInUnixSeconds, publication.impactUnavailableReason);
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0F);
    ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

/// The Name cell: the entry's icon (#1800), then a row-wide selectable (left or right click selects
/// it) and the name, in the warning colour when the program is missing and cut short with an ellipsis
/// when the column is too narrow (#1920), and its action menu. Returns whether the entry's tooltip is
/// due (straight away for a missing program, after the usual delay otherwise); the row draws it once
/// its cells are drawn, unless a clipped cell's own tooltip shows instead.
[[nodiscard]] bool renderNameCell(
    const Platform::StartupEntry& entry, bool missing, const UI::ColorScheme& scheme, StartupViewState& state, const RowExtras& extras)
{
    if (extras.icons != nullptr)
    {
        extras.icons->drawFileInline(extras.iconKey, startupIconPath(entry), entry.name);
    }
    if (ImGui::Selectable("##row", isSelected(entry, state), ImGuiSelectableFlags_SpanAllColumns) ||
        ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        state.selectedName = startupEntryKey(entry);
        state.selectedLocation = entry.location;
        state.selectedFamily = entry.packageFamily;
    }
    const bool tooltipDue = ImGui::IsItemHovered(missing ? ImGuiHoveredFlags_None : ImGuiHoveredFlags_DelayNormal);
    if (extras.actions != nullptr)
    {
        extras.actions->renderContextMenu(entry);
    }
    ImGui::SameLine(0.0F, 0.0F);
    if (missing)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textWarning);
    }
    // The entry's tooltip carries the full name.
    static_cast<void>(UI::Widgets::cellText(entry.name, {.clippedTooltip = false}));
    if (missing)
    {
        ImGui::PopStyleColor();
    }
    return tooltipDue;
}

/// The Command cell: in the warning colour when its program is missing; an unresolved shortcut shows
/// its own path, muted, since it has no command. Returns whether a clipped command's own tooltip shows.
[[nodiscard]] bool renderCommandCell(const Platform::StartupEntry& entry, bool missing, const UI::ColorScheme& scheme)
{
    if (missing)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textWarning);
        const bool tooltip = UI::Widgets::cellText(entry.command).tooltipShown;
        ImGui::PopStyleColor();
        return tooltip;
    }
    if (entry.command.empty() && !entry.sourcePath.empty())
    {
        // Rare (a shortcut whose target can't be resolved), so built when drawn.
        const std::string text = entry.sourcePath + " (target unresolved)";
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
        const bool tooltip = UI::Widgets::cellText(text).tooltipShown;
        ImGui::PopStyleColor();
        return tooltip;
    }
    return UI::Widgets::cellText(entry.command).tooltipShown;
}

/// One table row. `enabledLabel` is the cached Enabled text (built once per sample); it is drawn only
/// where the platform reports an enabled state. A clipped cell ends in an ellipsis with its full text
/// as its tooltip (#1920), which stands in for the entry's tooltip while it shows.
void renderStartupRow(const Platform::StartupEntry& entry,
                      const std::string& enabledLabel,
                      const Platform::StartupCapabilities& capabilities,
                      const UI::ColorScheme& scheme,
                      StartupViewState& state,
                      const RowExtras& extras)
{
    const bool missing = entry.target == Platform::StartupTargetState::Missing;
    ImGui::TableNextColumn();
    const bool rowTooltipDue = renderNameCell(entry, missing, scheme, state, extras);
    bool cellTooltip = false;
    ImGui::TableNextColumn();
    cellTooltip |= UI::Widgets::cellText(entry.publisher).tooltipShown;
    ImGui::TableNextColumn();
    if (capabilities.hasEnabledState)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, entry.enabled ? scheme.textPrimary : scheme.textMuted);
        cellTooltip |= UI::Widgets::cellText(enabledLabel).tooltipShown;
        ImGui::PopStyleColor();
    }
    ImGui::TableNextColumn();
    if (extras.impact != nullptr)
    {
        cellTooltip |= renderImpactCell(*extras.impact, scheme);
    }
    ImGui::TableNextColumn();
    cellTooltip |= UI::Widgets::cellText(startupScopeLabel(entry.scope)).tooltipShown;
    ImGui::TableNextColumn();
    cellTooltip |= UI::Widgets::cellText(startupLocationLabel(entry.location)).tooltipShown;
    ImGui::TableNextColumn();
    cellTooltip |= renderCommandCell(entry, missing, scheme);
    if (rowTooltipDue && !cellTooltip)
    {
        if (extras.impact != nullptr && extras.publication != nullptr &&
            ImGui::TableGetHoveredColumn() == static_cast<int>(StartupColumn::Impact))
        {
            renderImpactTooltip(*extras.impact, *extras.publication);
        }
        else
        {
            renderStartupTooltip(entry);
        }
    }
}

/// The visible rows only (ImGuiListClipper), in the order state.rows holds: only they look up icons.
void renderStartupRows(const Domain::StartupPublication& publication,
                       const Platform::StartupCapabilities& capabilities,
                       StartupViewState& state,
                       const UI::ColorScheme& scheme,
                       StartupActionsView* actions,
                       ProcessIconView* icons)
{
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(state.rows.size()));
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
        {
            const std::size_t index = state.rows[static_cast<std::size_t>(row)];
            const auto& entry = publication.entries[index];
            ImGui::TableNextRow();
            // Keyed by the entry (location, package family and registered name), not the row: a re-sort
            // or a new sample must not move an open row menu onto another entry.
            ImGui::PushID(static_cast<int>(entry.location));
            const std::string_view key = startupEntryKey(entry);
            ImGui::PushID(entry.packageFamily.c_str());
            ImGui::PushID(key.data(), key.data() + key.size());
            const RowExtras extras{.actions = actions,
                                   .icons = icons,
                                   .impact = (capabilities.hasImpact && index < publication.impacts.size()) ? &publication.impacts[index]
                                                                                                            : nullptr,
                                   .publication = &publication,
                                   .iconKey = state.iconKeys[index]};
            renderStartupRow(entry, state.enabledLabels[index], capabilities, scheme, state, extras);
            ImGui::PopID();
            ImGui::PopID();
            ImGui::PopID();
        }
    }
}

} // namespace

StartupViewContent renderStartupView(const Domain::StartupPublication* publication,
                                     const Platform::StartupCapabilities& capabilities,
                                     StartupViewState& state,
                                     StartupActionsView* actions,
                                     ProcessIconView* icons)
{
    if (!capabilities.canEnumerate)
    {
        renderUnsupported(capabilities, state);
        return StartupViewContent::Unsupported;
    }
    if (publication == nullptr || publication->version == 0)
    {
        UI::Widgets::renderEmptyState(ICON_FA_POWER_OFF "  Reading startup apps...");
        return StartupViewContent::Loading;
    }

    const auto& scheme = UI::Theme::get().scheme();
    renderFilterBar(*publication, state, scheme);
    if (actions != nullptr && actions->supported())
    {
        // Looked up by name and location only while a row is selected, so a re-sort or a new sample
        // keeps it; the list is a few dozen entries.
        const Platform::StartupEntry* selected = nullptr;
        if (!state.selectedName.empty())
        {
            const auto found =
                std::ranges::find_if(publication->entries, [&state](const Platform::StartupEntry& e) { return isSelected(e, state); });
            selected = (found != publication->entries.end()) ? &*found : nullptr;
        }
        actions->renderActionBar(selected);
        actions->renderResultLine();
    }

    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                                            ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                            ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                            ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##StartupTable", 7, TABLE_FLAGS))
    {
        return StartupViewContent::Table;
    }
    setupStartupColumns(capabilities);
    applySortSpecs(state);
    rebuildRowsIfStale(*publication, state); // a header click may have changed the order
    renderStartupRows(*publication, capabilities, state, scheme, actions, icons);
    ImGui::EndTable();
    return StartupViewContent::Table;
}
} // namespace App
