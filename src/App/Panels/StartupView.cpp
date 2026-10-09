#include "StartupView.h"

#include "App/Panels/ProcessTableLayout.h"
#include "App/Panels/StartupActionsView.h"
#include "Domain/StartupModel.h"
#include "Platform/IStartupProbe.h"
#include "UI/EmptyState.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>
// imgui_stdlib.h extends imgui.h's API for std::string, so it follows it.
#include <algorithm>
#include <cctype>
#include <compare>
#include <cstddef>
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

[[nodiscard]] bool containsIgnoringCase(std::string_view text, std::string_view needle)
{
    const auto lower = [](unsigned char c)
    {
        return std::tolower(c);
    };
    return !std::ranges::search(text, needle, {}, lower, lower).empty();
}

[[nodiscard]] std::strong_ordering compareIgnoringCase(std::string_view a, std::string_view b)
{
    return std::lexicographical_compare_three_way(
        a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) { return std::tolower(x) <=> std::tolower(y); });
}

[[nodiscard]] std::strong_ordering compareBy(const Platform::StartupEntry& a, const Platform::StartupEntry& b, StartupColumn column)
{
    switch (column)
    {
    case StartupColumn::Publisher:
        return compareIgnoringCase(a.publisher, b.publisher);
    case StartupColumn::Enabled:
        // Enabled first, then disabled ones by when they were disabled.
        if (a.enabled != b.enabled)
        {
            return a.enabled ? std::strong_ordering::less : std::strong_ordering::greater;
        }
        return a.disabledAtUnixSeconds <=> b.disabledAtUnixSeconds;
    case StartupColumn::Scope:
        return a.scope <=> b.scope;
    case StartupColumn::Location:
        return a.location <=> b.location;
    case StartupColumn::Command:
        return compareIgnoringCase(a.command, b.command);
    case StartupColumn::Name:
    default:
        return compareIgnoringCase(a.name, b.name);
    }
}

void renderText(std::string_view text)
{
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

/// The entry's source, command and resolved program, and why it is flagged when its program is missing.
void renderStartupTooltip(const Platform::StartupEntry& entry)
{
    const auto& scheme = UI::Theme::get().scheme();
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0F);
    ImGui::TextUnformatted(entry.name.c_str());
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
        for (const auto& entry : publication.entries)
        {
            state.enabledLabels.push_back(startupEnabledLabel(entry));
        }
    }
    state.rows = buildStartupRows(publication.entries, state.filter, state.sortColumn, state.ascending);
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
    default:
        return "";
    }
}

std::string_view startupScopeLabel(Platform::StartupScope scope) noexcept
{
    return scope == Platform::StartupScope::Machine ? "All users" : "Current user";
}

std::string startupEnabledLabel(const Platform::StartupEntry& entry)
{
    if (entry.enabled)
    {
        return "Enabled";
    }
    const std::string when = UI::Format::formatEpochDateTime(entry.disabledAtUnixSeconds);
    // "YYYY-MM-DD HH:MM:SS" in local time: the column shows the date.
    return when.empty() ? std::string("Disabled") : "Disabled since " + when.substr(0, when.find(' '));
}

std::vector<std::size_t>
buildStartupRows(std::span<const Platform::StartupEntry> entries, std::string_view filter, StartupColumn column, bool ascending)
{
    std::vector<std::size_t> rows;
    rows.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const auto& entry = entries[i];
        if (filter.empty() || containsIgnoringCase(entry.name, filter) || containsIgnoringCase(entry.publisher, filter) ||
            containsIgnoringCase(entry.command, filter))
        {
            rows.push_back(i);
        }
    }
    std::ranges::stable_sort(rows,
                             [&](std::size_t a, std::size_t b)
                             {
                                 const std::strong_ordering order = compareBy(entries[a], entries[b], column);
                                 return ascending ? (order < 0) : (order > 0);
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
    ImGui::InputTextWithHint("##StartupFilter", FILTER_HINT, &state.filter);
    ImGui::PopStyleColor();
    rebuildRowsIfStale(publication, state);
    ImGui::SameLine();
    ImGui::TextColored(scheme.textMuted, "%zu of %zu startup apps", state.rows.size(), publication.entries.size());
}

/// The table's columns, in StartupColumn order, each tagged with its StartupColumn as the user ID the
/// sort specs report; the header row is frozen while scrolling.
void setupStartupColumns()
{
    const float em = ImGui::GetFontSize();
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort, em * 14.0F, static_cast<ImGuiID>(StartupColumn::Name));
    ImGui::TableSetupColumn("Publisher", ImGuiTableColumnFlags_None, em * 12.0F, static_cast<ImGuiID>(StartupColumn::Publisher));
    ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_None, em * 11.0F, static_cast<ImGuiID>(StartupColumn::Enabled));
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
    return entry.location == state.selectedLocation && entry.name == state.selectedName;
}

/// The Name cell: a row-wide selectable (left or right click selects it), in the warning colour when
/// the program is missing, with the entry's tooltip (straight away for a missing program, after the
/// usual delay otherwise) and its action menu.
void renderNameCell(
    const Platform::StartupEntry& entry, bool missing, const UI::ColorScheme& scheme, StartupViewState& state, StartupActionsView* actions)
{
    if (missing)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textWarning);
    }
    if (ImGui::Selectable(entry.name.c_str(), isSelected(entry, state), ImGuiSelectableFlags_SpanAllColumns) ||
        ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        state.selectedName = entry.name;
        state.selectedLocation = entry.location;
    }
    if (missing)
    {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered(missing ? ImGuiHoveredFlags_None : ImGuiHoveredFlags_DelayNormal))
    {
        renderStartupTooltip(entry);
    }
    if (actions != nullptr)
    {
        actions->renderContextMenu(entry);
    }
}

/// The Command cell: in the warning colour when its program is missing; an unresolved shortcut shows
/// its own path, muted, since it has no command.
void renderCommandCell(const Platform::StartupEntry& entry, bool missing, const UI::ColorScheme& scheme)
{
    if (missing)
    {
        ImGui::TextColored(scheme.textWarning, "%s", entry.command.c_str());
    }
    else if (entry.command.empty() && !entry.sourcePath.empty())
    {
        ImGui::TextColored(scheme.textMuted, "%s (target unresolved)", entry.sourcePath.c_str());
    }
    else
    {
        ImGui::TextUnformatted(entry.command.c_str());
    }
}

/// One table row. `enabledLabel` is the cached Enabled text (built once per sample); it is drawn only
/// where the platform reports an enabled state.
void renderStartupRow(const Platform::StartupEntry& entry,
                      const std::string& enabledLabel,
                      const Platform::StartupCapabilities& capabilities,
                      const UI::ColorScheme& scheme,
                      StartupViewState& state,
                      StartupActionsView* actions)
{
    const bool missing = entry.target == Platform::StartupTargetState::Missing;
    ImGui::TableNextColumn();
    renderNameCell(entry, missing, scheme, state, actions);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(entry.publisher.c_str());
    ImGui::TableNextColumn();
    if (capabilities.hasEnabledState)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, entry.enabled ? scheme.textPrimary : scheme.textMuted);
        ImGui::TextUnformatted(enabledLabel.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::TableNextColumn();
    renderText(startupScopeLabel(entry.scope));
    ImGui::TableNextColumn();
    renderText(startupLocationLabel(entry.location));
    ImGui::TableNextColumn();
    renderCommandCell(entry, missing, scheme);
}

/// The visible rows only (ImGuiListClipper), in the order state.rows holds.
void renderStartupRows(const Domain::StartupPublication& publication,
                       const Platform::StartupCapabilities& capabilities,
                       StartupViewState& state,
                       const UI::ColorScheme& scheme,
                       StartupActionsView* actions)
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
            // Keyed by the entry (location and name), not the row: a re-sort or a new sample must not
            // move an open row menu onto another entry.
            ImGui::PushID(static_cast<int>(entry.location));
            ImGui::PushID(entry.name.c_str());
            renderStartupRow(entry, state.enabledLabels[index], capabilities, scheme, state, actions);
            ImGui::PopID();
            ImGui::PopID();
        }
    }
}

} // namespace

StartupViewContent renderStartupView(const Domain::StartupPublication* publication,
                                     const Platform::StartupCapabilities& capabilities,
                                     StartupViewState& state,
                                     StartupActionsView* actions)
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
    if (!ImGui::BeginTable("##StartupTable", 6, TABLE_FLAGS))
    {
        return StartupViewContent::Table;
    }
    setupStartupColumns();
    applySortSpecs(state);
    rebuildRowsIfStale(*publication, state); // a header click may have changed the order
    renderStartupRows(*publication, capabilities, state, scheme, actions);
    ImGui::EndTable();
    return StartupViewContent::Table;
}
} // namespace App
