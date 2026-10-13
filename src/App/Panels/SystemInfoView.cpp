#include "SystemInfoView.h"

#include "App/Panels/ProcessTableLayout.h"
#include "App/Panels/SystemInfoSections.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"
#include "UI/ChromeWidgets.h"
#include "UI/EmptyState.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/LineLayout.h"
#include "UI/Sparkline.h"
#include "UI/Theme.h"

#include <imgui.h>
// imgui_stdlib.h extends imgui.h's API for std::string, so it follows it.
#include <algorithm>
#include <cstddef>
#include <map>
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
void rebuildIfStale(const Domain::SystemInfoSnapshot& snapshot,
                    const Platform::SystemInfoCapabilities& capabilities,
                    SystemInfoViewState& state)
{
    if (state.sectionsVersion != snapshot.version)
    {
        state.sections = SystemInfo::buildSystemInfoSections(snapshot, state.host, capabilities);
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

/// A row's button and its note, muted: "Show Wi-Fi details" (#1782) or "Show public IP" (#1518).
/// Disabled while a read is in flight or once clicked. Returns the row's action when it was clicked,
/// else RowAction::None.
[[nodiscard]] SystemInfo::RowAction
renderRowAction(const SystemInfo::Row& row, bool reading, SystemInfoViewState& state, const UI::ColorScheme& scheme)
{
    const char* label = nullptr;
    const char* tooltip = nullptr;
    std::string_view note;
    bool* clickedFlag = nullptr;
    switch (row.action)
    {
    case SystemInfo::RowAction::None:
        return SystemInfo::RowAction::None;
    case SystemInfo::RowAction::ShowWifiDetails:
        label = ICON_FA_WIFI "  Show Wi-Fi details";
        tooltip = "Read the network name, signal quality, standard, band and rates, now and on every Refresh";
        note = SystemInfo::WIFI_DETAILS_NOTE;
        clickedFlag = &state.wifiDetailsClicked;
        break;
    case SystemInfo::RowAction::ShowPublicIp:
        label = ICON_FA_NETWORK_WIRED "  Show public IP";
        tooltip = "Ask OpenDNS's resolver (myip.opendns.com) for this machine's public IPv4 and IPv6 addresses, now and "
                  "on every Refresh";
        note = SystemInfo::PUBLIC_IP_NOTE;
        clickedFlag = &state.publicIpClicked;
        break;
    }
    ImGui::BeginDisabled(reading || *clickedFlag);
    const bool clicked = ImGui::SmallButton(label);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", tooltip);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
    textView(note);
    ImGui::PopStyleColor();
    if (!clicked)
    {
        return SystemInfo::RowAction::None;
    }
    *clickedFlag = true;
    return row.action;
}

/// One value cell: the value (with its valueTooltip, if any), or a muted em dash whose tooltip says why
/// there is none.
void renderValue(const SystemInfo::Row& row, const UI::ColorScheme& scheme)
{
    if (row.available())
    {
        textView(row.value);
        if (!row.valueTooltip.empty() && ImGui::BeginItemTooltip())
        {
            textView(row.valueTooltip);
            ImGui::EndTooltip();
        }
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

/// A sensor kind's chart colour: the GPU tab's for temperature and fan speed, the power charts' for
/// power, and accents for the rest.
[[nodiscard]] const ImVec4& sensorColor(Platform::SensorKind kind, const UI::ColorScheme& scheme)
{
    switch (kind)
    {
    case Platform::SensorKind::Temperature:
        return scheme.gpuTemperature;
    case Platform::SensorKind::Fan:
        return scheme.gpuFan;
    case Platform::SensorKind::Power:
        return scheme.chartPower;
    case Platform::SensorKind::Voltage:
        return scheme.accents[0];
    case Platform::SensorKind::Current:
        return scheme.accents[1];
    }
    return scheme.accents[0];
}

/// The width of the Sensors section's sparkline column, in ems.
constexpr float SPARKLINE_WIDTH_EM = 10.0F;

/// A section: its header with a Copy button at the right, then a two-column table of its visible rows.
/// Every section's label column is @p labelWidth wide, so the values line up down the page.
/// Returns the action of a row button that was clicked (#1782, #1518), else RowAction::None.
[[nodiscard]] SystemInfo::RowAction renderSection(const SystemInfo::VisibleSection& visible,
                                                  float labelWidth,
                                                  const std::map<std::string, Domain::SensorHistory>& sensorHistories,
                                                  bool reading,
                                                  SystemInfoViewState& state,
                                                  const UI::ColorScheme& scheme)
{
    SystemInfo::RowAction clickedAction = SystemInfo::RowAction::None;
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

    // The Sensors section (#1522) has a third column: each sensor's recent history as a sparkline.
    const auto history = [&](const SystemInfo::Row& row) -> const Domain::SensorHistory*
    {
        if (!row.sensorKind.has_value())
        {
            return nullptr;
        }
        const auto it = sensorHistories.find(row.label);
        return (it != sensorHistories.end() && !it->second.times.empty()) ? &it->second : nullptr;
    };
    const bool charts = std::ranges::any_of(visible.rows, [&](std::size_t index) { return history(section.rows[index]) != nullptr; });
    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##Rows", charts ? 3 : 2, TABLE_FLAGS))
    {
        const float sparklineWidth = ImGui::GetFontSize() * SPARKLINE_WIDTH_EM;
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        if (charts)
        {
            ImGui::TableSetupColumn("History", ImGuiTableColumnFlags_WidthFixed, sparklineWidth);
        }
        for (const std::size_t index : visible.rows)
        {
            const SystemInfo::Row& row = section.rows[index];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
            textView(row.label);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            if (row.action == SystemInfo::RowAction::None)
            {
                renderValue(row, scheme);
            }
            else
            {
                ImGui::PushID(static_cast<int>(index));
                if (const SystemInfo::RowAction clicked = renderRowAction(row, reading, state, scheme);
                    clicked != SystemInfo::RowAction::None)
                {
                    clickedAction = clicked;
                }
                ImGui::PopID();
            }
            const Domain::SensorHistory* recent = charts ? history(row) : nullptr;
            if (charts && ImGui::TableNextColumn() && recent != nullptr)
            {
                ImGui::PushID(static_cast<int>(index));
                UI::Widgets::renderSparkline("##History",
                                             recent->times,
                                             recent->values,
                                             static_cast<double>(Domain::Sampling::HISTORY_SECONDS_DEFAULT),
                                             ImVec2(sparklineWidth, ImGui::GetTextLineHeight()),
                                             sensorColor(row.sensorKind.value_or(Platform::SensorKind::Temperature), scheme));
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
    ImGui::Spacing();
    return clickedAction;
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
        return {.content = SystemInfoViewContent::Unsupported,
                .refreshRequested = false,
                .wifiDetailsRequested = false,
                .publicIpRequested = false};
    }
    if (snapshot == nullptr || snapshot->version == 0)
    {
        UI::Widgets::renderEmptyState(ICON_FA_SERVER "  Reading system information...");
        return {.content = SystemInfoViewContent::Loading,
                .refreshRequested = false,
                .wifiDetailsRequested = false,
                .publicIpRequested = false};
    }

    const auto& scheme = UI::Theme::get().scheme();
    rebuildIfStale(*snapshot, capabilities, state); // Copy all copies the current read
    const bool refresh = renderToolbar(reading, state, scheme);
    rebuildIfStale(*snapshot, capabilities, state); // the filter or toggle may have changed
    ImGui::Spacing();

    bool wifiDetails = false;
    bool publicIp = false;
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
            const SystemInfo::RowAction clicked = renderSection(visible, labelWidth, snapshot->sensorHistories, reading, state, scheme);
            wifiDetails = wifiDetails || clicked == SystemInfo::RowAction::ShowWifiDetails;
            publicIp = publicIp || clicked == SystemInfo::RowAction::ShowPublicIp;
        }
        if (state.visible.empty())
        {
            ImGui::TextColored(scheme.textMuted, "Nothing matches \"%s\"", state.filter.c_str());
        }
    }
    ImGui::EndChild();
    return {.content = SystemInfoViewContent::Sections,
            .refreshRequested = refresh,
            .wifiDetailsRequested = wifiDetails,
            .publicIpRequested = publicIp};
}

} // namespace App
