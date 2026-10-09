#include "SystemInfoView.h"

#include "App/Panels/ProcessTableLayout.h"
#include "App/Panels/SystemInfoSections.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"
#include "UI/ChromeWidgets.h"
#include "UI/EmptyState.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/LineLayout.h"
#include "UI/Theme.h"

#include <imgui.h>
// imgui_stdlib.h extends imgui.h's API for std::string, so it follows it.
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <misc/cpp/imgui_stdlib.h>

namespace App
{

namespace
{

constexpr const char* FILTER_HINT = ICON_FA_MAGNIFYING_GLASS "  Filter system information...";

void textView(std::string_view text)
{
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

/// Rebuilds the sections when a new read is published, and the visible rows when that, the filter or
/// the identifier toggle changed: never every frame (#580).
void rebuildIfStale(const Domain::SystemInfoSnapshot& snapshot, SystemInfoViewState& state)
{
    if (state.sectionsVersion != snapshot.version)
    {
        state.sections = SystemInfo::buildSystemInfoSections(snapshot, state.host);
        state.sectionsVersion = snapshot.version;
        const std::string when = UI::Format::formatEpochDateTime(snapshot.readAtUnixSeconds);
        state.readAtText = when.empty() ? std::string{} : "Read at " + when.substr(when.find(' ') + 1);
    }
    if (state.visibleVersion == state.sectionsVersion && state.visibleFilter == state.filter &&
        state.visibleShowIdentifiers == state.showIdentifiers)
    {
        return;
    }
    state.visible = SystemInfo::visibleSections(state.sections, state.filter, state.showIdentifiers);
    state.visibleVersion = state.sectionsVersion;
    state.visibleFilter = state.filter;
    state.visibleShowIdentifiers = state.showIdentifiers;
}

void copy(std::string text, SystemInfoViewState& state)
{
    state.copiedText = std::move(text);
    ImGui::SetClipboardText(state.copiedText.c_str());
}

/// The filter box, Show identifiers, Copy all, Refresh and when the facts were read. Returns whether
/// Refresh was clicked.
bool renderToolbar(bool reading, SystemInfoViewState& state, const UI::ColorScheme& scheme)
{
    ImGui::SetNextItemWidth(ProcessTableLayout::computeFilterWidth(
        ImGui::CalcTextSize(FILTER_HINT).x, ImGui::GetStyle().FramePadding.x, ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, scheme.textMuted);
    ImGui::InputTextWithHint("##SystemInfoFilter", FILTER_HINT, &state.filter);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Checkbox("Show identifiers", &state.showIdentifiers);
    ImGui::SetItemTooltip("Show the user, computer and domain names, and include them when copying");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_COPY "  Copy all"))
    {
        copy(SystemInfo::allSectionsText(state.sections, state.showIdentifiers), state);
    }
    ImGui::SetItemTooltip("Copy every section as plain text%s", state.showIdentifiers ? "" : " (identifiers left out)");
    ImGui::SameLine();
    ImGui::BeginDisabled(reading);
    const bool refresh = ImGui::Button(ICON_FA_ARROWS_ROTATE "  Refresh");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
    textView(reading ? std::string_view{"Reading..."} : std::string_view{state.readAtText});
    ImGui::PopStyleColor();
    return refresh;
}

/// One value cell: the value, or a muted em dash whose tooltip says why there is none.
void renderValue(const SystemInfo::Row& row, const UI::ColorScheme& scheme)
{
    if (row.available())
    {
        textView(row.value);
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
    textView(SystemInfo::UNAVAILABLE_TEXT);
    ImGui::PopStyleColor();
    if (!row.unavailableReason.empty() && ImGui::BeginItemTooltip())
    {
        textView(row.unavailableReason);
        ImGui::EndTooltip();
    }
}

/// A section: its header with a Copy button at the right, then a two-column table of its visible rows.
/// Every section's label column is @p labelWidth wide, so the values line up down the page.
void renderSection(const SystemInfo::VisibleSection& visible, float labelWidth, SystemInfoViewState& state, const UI::ColorScheme& scheme)
{
    const SystemInfo::Section& section = state.sections[visible.section];
    ImGui::PushID(static_cast<int>(visible.section));
    (void) UI::Widgets::sectionHeader(section.icon, section.title);
    const char* copyLabel = ICON_FA_COPY "  Copy";
    const float copyWidth = ImGui::CalcTextSize(copyLabel).x + (ImGui::GetStyle().FramePadding.x * 2.0F);
    ImGui::SameLine(std::max(ImGui::GetContentRegionMax().x - copyWidth, 0.0F));
    if (ImGui::SmallButton(copyLabel))
    {
        copy(SystemInfo::sectionText(section, state.showIdentifiers), state);
    }
    ImGui::SetItemTooltip("Copy this section as plain text");

    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##Rows", 2, TABLE_FLAGS))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        for (const std::size_t index : visible.rows)
        {
            const SystemInfo::Row& row = section.rows[index];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
            textView(row.label);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            renderValue(row, scheme);
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
    ImGui::Spacing();
}

} // namespace

SystemInfoViewResult renderSystemInfoView(const Domain::SystemInfoSnapshot* snapshot,
                                          const Platform::SystemInfoCapabilities& capabilities,
                                          bool reading,
                                          SystemInfoViewState& state)
{
    if (!capabilities.hasOs)
    {
        const std::string heading = std::string(ICON_FA_SERVER "  ") + capabilities.unavailableReason;
        UI::Widgets::renderEmptyState(heading.c_str());
        return {.content = SystemInfoViewContent::Unsupported, .refreshRequested = false};
    }
    if (snapshot == nullptr || snapshot->version == 0)
    {
        UI::Widgets::renderEmptyState(ICON_FA_SERVER "  Reading system information...");
        return {.content = SystemInfoViewContent::Loading, .refreshRequested = false};
    }

    const auto& scheme = UI::Theme::get().scheme();
    rebuildIfStale(*snapshot, state); // Copy all copies the current read
    const bool refresh = renderToolbar(reading, state, scheme);
    rebuildIfStale(*snapshot, state); // the filter or toggle may have changed
    ImGui::Spacing();

    if (ImGui::BeginChild("##SystemInfoSections", ImVec2(0.0F, 0.0F)))
    {
        float widestLabel = 0.0F;
        for (const auto& visible : state.visible)
        {
            for (const std::size_t index : visible.rows)
            {
                widestLabel = std::max(widestLabel, ImGui::CalcTextSize(state.sections[visible.section].rows[index].label.c_str()).x);
            }
        }
        const float labelWidth = UI::LineLayout::labelColumnWidth(widestLabel, ImGui::GetFontSize());
        for (const auto& visible : state.visible)
        {
            renderSection(visible, labelWidth, state, scheme);
        }
        if (state.visible.empty())
        {
            ImGui::TextColored(scheme.textMuted, "Nothing matches \"%s\"", state.filter.c_str());
        }
    }
    ImGui::EndChild();
    return {.content = SystemInfoViewContent::Sections, .refreshRequested = refresh};
}

} // namespace App
