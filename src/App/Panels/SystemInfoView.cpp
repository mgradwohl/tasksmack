#include "SystemInfoView.h"

#include "App/Panels/ProcessTableLayout.h"
#include "App/Panels/SystemInfoSections.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SensorModel.h"
#include "Domain/SensorTracker.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"
#include "UI/CellText.h"
#include "UI/ChromeWidgets.h"
#include "UI/EmptyState.h"
#include "UI/Format.h"
#include "UI/FrameOutline.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/LineLayout.h"
#include "UI/Sparkline.h"
#include "UI/Theme.h"

#include <imgui.h>
// imgui_stdlib.h extends imgui.h's API for std::string, so it follows it.
#include <algorithm>
#include <cstddef>
#include <cstdint>
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
constexpr const char* SHOW_IDENTIFIERS_LABEL = "Show identifiers";
constexpr const char* COPY_ALL_LABEL = ICON_FA_COPY "  Copy all";
constexpr const char* SAVE_REPORT_LABEL = ICON_FA_FLOPPY_DISK "  Save report...";
constexpr const char* REFRESH_LABEL = ICON_FA_ARROWS_ROTATE "  Refresh";
constexpr const char* READING_TEXT = "Reading...";
/// The read time's label, dropped when the toolbar is too narrow for it (#1922).
constexpr std::string_view READ_AT_PREFIX = "Read at ";

void textView(std::string_view text)
{
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

/// For the charts before the sensor sampler's first read. A function-local static: a namespace-scope map's
/// constructor may throw (MSVC's does) before main().
[[nodiscard]] const std::map<std::string, Domain::SensorHistory>& noSensorHistories()
{
    static const std::map<std::string, Domain::SensorHistory> NONE;
    return NONE;
}

/// Rebuilds the sections when a new page read or sensor read (#2737) is published, and the visible rows
/// when they, the filter or the identifier toggle changed: never every frame (#580).
void rebuildIfStale(const Domain::SystemInfoSnapshot& snapshot,
                    const Domain::SensorPublication* sensors,
                    const Platform::SystemInfoCapabilities& capabilities,
                    SystemInfoViewState& state)
{
    const std::uint64_t sensorsVersion = sensors != nullptr ? sensors->version : 0;
    const bool newRead = state.sectionsVersion != snapshot.version;
    if (!newRead && state.sectionsSensorsVersion != sensorsVersion)
    {
        // Only the sensors changed, at the Settings interval: just their section is built again, when it
        // is there and still has readings to show.
        const auto sensorsSection = std::ranges::find(state.sections, SystemInfo::SENSORS_SECTION_TITLE, &SystemInfo::Section::title);
        if (sensorsSection != state.sections.end() && sensors != nullptr && sensors->sensors.available)
        {
            *sensorsSection = SystemInfo::buildSensorsSection(sensors->sensors, sensors->ranges);
            state.sectionsSensorsVersion = sensorsVersion;
            ++state.sectionsGeneration;
        }
    }
    if (newRead || state.sectionsSensorsVersion != sensorsVersion)
    {
        state.sections = SystemInfo::buildSystemInfoSections(snapshot, state.host, capabilities, sensors);
        state.sectionsVersion = snapshot.version;
        state.sectionsSensorsVersion = sensorsVersion;
        ++state.sectionsGeneration;
    }
    if (newRead)
    {
        state.authenticateClicked = false; // a new read: offered again if the facts are still root-only
        const std::string when = UI::Format::formatEpochDateTime(snapshot.readAtUnixSeconds);
        state.readAtText = when.empty() ? std::string{} : std::string{READ_AT_PREFIX} + when.substr(when.find(' ') + 1);
    }
    if (state.visibleVersion == state.sectionsGeneration && state.visibleFilter == state.filter &&
        state.visibleShowIdentifiers == state.showIdentifiers)
    {
        return;
    }
    state.visible = SystemInfo::visibleSections(state.sections, state.filter, state.showIdentifiers);
    state.visibleVersion = state.sectionsGeneration;
    state.visibleFilter = state.filter;
    state.visibleShowIdentifiers = state.showIdentifiers;
}

void copy(std::string text, SystemInfoViewState& state)
{
    state.copiedText = std::move(text);
    ImGui::SetClipboardText(state.copiedText.c_str());
}

/// What the toolbar's buttons asked for this frame.
struct ToolbarClicks
{
    bool refresh = false;
    bool saveReport = false;
};

/// The filter box, Show identifiers, Copy all, Save report..., Refresh and when the facts were read;
/// under them, the last save's note. At narrow widths the box narrows, then the time drops its
/// "Read at" (#1922): see SystemInfo::computeToolbarLayout().
ToolbarClicks renderToolbar(bool reading, SystemInfoViewState& state, const UI::ColorScheme& scheme)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const auto textWidth = [](std::string_view text)
    {
        return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
    };
    const auto buttonWidth = [&](std::string_view label)
    {
        return textWidth(label) + (style.FramePadding.x * 2.0F);
    };
    const std::string_view readAtFull = reading ? std::string_view{READING_TEXT} : std::string_view{state.readAtText};
    const std::string_view readAtShort = readAtFull.starts_with(READ_AT_PREFIX) ? readAtFull.substr(READ_AT_PREFIX.size()) : readAtFull;
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    const float hintWidth = textWidth(FILTER_HINT);
    const float checkboxWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + textWidth(SHOW_IDENTIFIERS_LABEL);
    const SystemInfo::ToolbarLayout layout = SystemInfo::computeToolbarLayout({
        .rowPx = rowWidth,
        .filterWantedPx = ProcessTableLayout::computeFilterWidth(hintWidth, style.FramePadding.x, ImGui::GetFontSize(), rowWidth),
        .filterHintPx = hintWidth + (style.FramePadding.x * 2.0F),
        .controlsPx = (style.ItemSpacing.x * 5.0F) + checkboxWidth + buttonWidth(COPY_ALL_LABEL) + buttonWidth(SAVE_REPORT_LABEL) +
                      buttonWidth(REFRESH_LABEL),
        .readAtFullPx = textWidth(readAtFull),
        .readAtShortPx = textWidth(readAtShort),
    });
    state.readAtForm = layout.readAt;

    ImGui::SetNextItemWidth(layout.filterPx);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, scheme.textMuted);
    {
        const UI::Widgets::FrameOutlineScope frameOutline;
        ImGui::InputTextWithHint("##SystemInfoFilter", FILTER_HINT, &state.filter);
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    UI::Widgets::outlinedCheckbox(SHOW_IDENTIFIERS_LABEL, &state.showIdentifiers);
    ImGui::SetItemTooltip("Show what identifies this machine or its user (user, computer and domain names, serial numbers, "
                          "UUIDs, MAC and public IP addresses, Wi-Fi network names), and include it when copying or saving");
    ImGui::SameLine();
    if (ImGui::Button(COPY_ALL_LABEL))
    {
        copy(SystemInfo::allSectionsText(state.sections, state.showIdentifiers), state);
    }
    ImGui::SetItemTooltip("Copy every section as plain text%s", state.showIdentifiers ? "" : " (identifiers left out)");
    ImGui::SameLine();
    ImGui::BeginDisabled(state.saveDialogOpen);
    const bool saveReport = ImGui::Button(SAVE_REPORT_LABEL);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Save every section to a text or JSON file%s", state.showIdentifiers ? "" : " (identifiers left out)");
    ImGui::SameLine();
    ImGui::BeginDisabled(reading);
    const bool refresh = ImGui::Button(REFRESH_LABEL);
    ImGui::EndDisabled();
    if (layout.readAt != SystemInfo::ReadAtForm::NextLine)
    {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
    if (layout.readAt == SystemInfo::ReadAtForm::TimeOnly)
    {
        textView(readAtShort);
        if (ImGui::BeginItemTooltip())
        {
            textView(readAtFull);
            ImGui::EndTooltip();
        }
    }
    else
    {
        textView(readAtFull);
    }
    if (!state.saveNote.empty())
    {
        textView(state.saveNote); // its own line: a saved path can be long
    }
    ImGui::PopStyleColor();
    return {.refresh = refresh, .saveReport = saveReport};
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
    case SystemInfo::RowAction::AuthenticateToRead:
        label = ICON_FA_LOCK "  Authenticate to read";
        tooltip = "Ask for an administrator's password, then read what only root can read here through TaskSmack's helper: "
                  "the serial numbers, UUID, SMBIOS version, memory modules and every user's crashes";
        note = SystemInfo::AUTHENTICATE_NOTE;
        clickedFlag = &state.authenticateClicked;
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
/// there is none. A value too wide for the page ends in an ellipsis, the whole of it on hover (#1920).
/// Shows the redacted text of a row that names the user while identifiers are hidden (#2120).
void renderValue(const SystemInfo::Row& row, bool showIdentifiers, const UI::ColorScheme& scheme)
{
    if (row.available())
    {
        const std::string& value = row.shownValue(showIdentifiers);
        const std::string& valueTooltip = row.shownValueTooltip(showIdentifiers);
        const bool ownTooltip = !valueTooltip.empty();
        const UI::Widgets::CellTextResult drawn = UI::Widgets::cellText(value, {.clippedTooltip = !ownTooltip});
        if (ownTooltip && ImGui::BeginItemTooltip())
        {
            if (drawn.clipped)
            {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * UI::Widgets::CLIPPED_CELL_TOOLTIP_WRAP_EM);
                textView(value);
                ImGui::PopTextWrapPos();
            }
            textView(valueTooltip);
            ImGui::EndTooltip();
        }
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
    textView(SystemInfo::UNAVAILABLE_TEXT);
    ImGui::PopStyleColor();
    const std::string& reason = row.shownUnavailableReason(showIdentifiers);
    if (!reason.empty() && ImGui::BeginItemTooltip())
    {
        textView(reason);
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
                                                  double sensorHistorySeconds,
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
        const auto it = sensorHistories.find(row.sensorKey);
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
            textView(row.shownLabel(state.showIdentifiers));
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            if (row.action == SystemInfo::RowAction::None)
            {
                renderValue(row, state.showIdentifiers, scheme);
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
                                             sensorHistorySeconds,
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
                                          SystemInfoViewState& state,
                                          const Domain::SensorPublication* sensors)
{
    if (!capabilities.hasOs)
    {
        const std::string heading = std::string(ICON_FA_SERVER "  ") + capabilities.unavailableReason;
        UI::Widgets::renderEmptyState(heading.c_str());
        return {.content = SystemInfoViewContent::Unsupported,
                .refreshRequested = false,
                .wifiDetailsRequested = false,
                .publicIpRequested = false,
                .authenticateRequested = false,
                .saveReportRequested = false};
    }
    if (snapshot == nullptr || snapshot->version == 0)
    {
        UI::Widgets::renderEmptyState(ICON_FA_SERVER "  Reading system information...");
        return {.content = SystemInfoViewContent::Loading,
                .refreshRequested = false,
                .wifiDetailsRequested = false,
                .publicIpRequested = false,
                .authenticateRequested = false,
                .saveReportRequested = false};
    }

    const auto& scheme = UI::Theme::get().scheme();
    rebuildIfStale(*snapshot, sensors, capabilities, state); // Copy all copies the current read
    const ToolbarClicks clicks = renderToolbar(reading, state, scheme);
    rebuildIfStale(*snapshot, sensors, capabilities, state); // the filter or toggle may have changed
    ImGui::Spacing();

    bool wifiDetails = false;
    bool publicIp = false;
    bool authenticate = false;
    if (ImGui::BeginChild("##SystemInfoSections", ImVec2(0.0F, 0.0F)))
    {
        float widestLabel = 0.0F;
        for (const auto& visible : state.visible)
        {
            for (const std::size_t index : visible.rows)
            {
                widestLabel =
                    std::max(widestLabel,
                             ImGui::CalcTextSize(state.sections[visible.section].rows[index].shownLabel(state.showIdentifiers).c_str()).x);
            }
        }
        const float labelWidth = UI::LineLayout::labelColumnWidth(widestLabel, ImGui::GetFontSize());
        for (const auto& visible : state.visible)
        {
            const SystemInfo::RowAction clicked =
                renderSection(visible,
                              labelWidth,
                              sensors != nullptr ? sensors->histories : noSensorHistories(),
                              sensors != nullptr ? sensors->historySeconds : Domain::Sampling::HISTORY_SECONDS_DEFAULT,
                              reading,
                              state,
                              scheme);
            wifiDetails = wifiDetails || clicked == SystemInfo::RowAction::ShowWifiDetails;
            publicIp = publicIp || clicked == SystemInfo::RowAction::ShowPublicIp;
            authenticate = authenticate || clicked == SystemInfo::RowAction::AuthenticateToRead;
        }
        if (state.visible.empty())
        {
            ImGui::TextColored(scheme.textMuted, "Nothing matches \"%s\"", state.filter.c_str());
        }
    }
    ImGui::EndChild();
    return {.content = SystemInfoViewContent::Sections,
            .refreshRequested = clicks.refresh,
            .wifiDetailsRequested = wifiDetails,
            .publicIpRequested = publicIp,
            .authenticateRequested = authenticate,
            .saveReportRequested = clicks.saveReport};
}

} // namespace App
