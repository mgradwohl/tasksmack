#include "ServicesView.h"

#include "App/Panels/ProcessTableLayout.h"
#include "Domain/ServiceModel.h"
#include "Platform/IServiceProbe.h"
#include "UI/EmptyState.h"
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

constexpr const char* FILTER_HINT = ICON_FA_MAGNIFYING_GLASS "  Filter services...";

[[nodiscard]] bool containsIgnoringCase(std::string_view text, std::string_view needle)
{
    const auto lower = [](unsigned char c)
    {
        return std::tolower(c);
    };
    return !std::ranges::search(text, needle, {}, lower, lower).empty();
}

/// Case-insensitive three-way comparison of two strings.
[[nodiscard]] std::strong_ordering compareIgnoringCase(std::string_view a, std::string_view b)
{
    return std::lexicographical_compare_three_way(
        a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) { return std::tolower(x) <=> std::tolower(y); });
}

[[nodiscard]] std::strong_ordering compareBy(const Platform::ServiceInfo& a, const Platform::ServiceInfo& b, ServiceColumn column)
{
    switch (column)
    {
    case ServiceColumn::DisplayName:
        return compareIgnoringCase(a.displayName, b.displayName);
    case ServiceColumn::State:
        return compareIgnoringCase(serviceStateLabel(a.state), serviceStateLabel(b.state));
    case ServiceColumn::StartType:
        return compareIgnoringCase(serviceStartTypeLabel(a.startType), serviceStartTypeLabel(b.startType));
    case ServiceColumn::Pid:
        return a.pid <=> b.pid;
    case ServiceColumn::Account:
        return compareIgnoringCase(a.account, b.account);
    case ServiceColumn::Name:
    default:
        return compareIgnoringCase(a.name, b.name);
    }
}

/// The description, command line and group, as the row's tooltip.
void renderServiceTooltip(const Platform::ServiceInfo& service)
{
    const auto& scheme = UI::Theme::get().scheme();
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0F);
    ImGui::TextUnformatted(service.displayName.empty() ? service.name.c_str() : service.displayName.c_str());
    if (!service.description.empty())
    {
        ImGui::TextColored(scheme.textMuted, "%s", service.description.c_str());
    }
    if (!service.binaryPath.empty())
    {
        ImGui::Text("Command: %s", service.binaryPath.c_str());
    }
    if (!service.group.empty())
    {
        ImGui::Text("Group: %s", service.group.c_str());
    }
    if (!service.serviceType.empty())
    {
        ImGui::Text("Type: %s", service.serviceType.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

/// Rebuilds the rows only when what they are built from changed: at most once per sample, sort or
/// filter edit, never every frame (#580).
void rebuildRowsIfStale(const Domain::ServicePublication& publication, ServicesViewState& state)
{
    if (state.rowsVersion == publication.version && state.rowsFilter == state.filter && state.rowsColumn == state.sortColumn &&
        state.rowsAscending == state.ascending)
    {
        return;
    }
    state.rows = buildServiceRows(publication.services, state.filter, state.sortColumn, state.ascending);
    state.rowsVersion = publication.version;
    state.rowsFilter = state.filter;
    state.rowsColumn = state.sortColumn;
    state.rowsAscending = state.ascending;
}

} // namespace

std::string_view serviceStateLabel(Platform::ServiceState state) noexcept
{
    using enum Platform::ServiceState;
    switch (state)
    {
    case Stopped:
        return "Stopped";
    case StartPending:
        return "Starting";
    case StopPending:
        return "Stopping";
    case Running:
        return "Running";
    case ContinuePending:
        return "Resuming";
    case PausePending:
        return "Pausing";
    case Paused:
        return "Paused";
    case Unknown:
    default:
        return "Unknown";
    }
}

std::string_view serviceStartTypeLabel(Platform::ServiceStartType startType) noexcept
{
    using enum Platform::ServiceStartType;
    switch (startType)
    {
    case Automatic:
        return "Automatic";
    case AutomaticDelayed:
        return "Automatic (delayed)";
    case Manual:
        return "Manual";
    case Disabled:
        return "Disabled";
    case Boot:
        return "Boot";
    case System:
        return "System";
    case Unknown:
    default:
        return "";
    }
}

ImVec4 serviceStateColor(Platform::ServiceState state, const UI::ColorScheme& scheme) noexcept
{
    using enum Platform::ServiceState;
    switch (state)
    {
    case Running:
        return scheme.statusRunning;
    case StartPending:
    case StopPending:
    case ContinuePending:
    case PausePending:
        return scheme.statusDiskSleep;
    case Paused:
        return scheme.statusStopped;
    case Stopped:
    case Unknown:
    default:
        return scheme.statusSleeping;
    }
}

std::vector<std::size_t>
buildServiceRows(std::span<const Platform::ServiceInfo> services, std::string_view filter, ServiceColumn column, bool ascending)
{
    std::vector<std::size_t> rows;
    rows.reserve(services.size());
    for (std::size_t i = 0; i < services.size(); ++i)
    {
        if (filter.empty() || containsIgnoringCase(services[i].name, filter) || containsIgnoringCase(services[i].displayName, filter))
        {
            rows.push_back(i);
        }
    }
    std::ranges::stable_sort(rows,
                             [&](std::size_t a, std::size_t b)
                             {
                                 const std::strong_ordering order = compareBy(services[a], services[b], column);
                                 return ascending ? (order < 0) : (order > 0);
                             });
    return rows;
}

ServicesViewContent renderServicesView(const Domain::ServicePublication* publication,
                                       const Platform::ServiceCapabilities& capabilities,
                                       ServicesViewState& state)
{
    if (!capabilities.canEnumerate)
    {
        // The probe's reason: the platform has no service list yet, or the service manager refused.
        // Built once: the reason is fixed when the probe is made.
        if (state.unavailableHeading.empty())
        {
            state.unavailableHeading =
                std::string(ICON_FA_GEARS "  ") +
                (capabilities.unavailableReason.empty() ? std::string("Services aren't available") : capabilities.unavailableReason);
        }
        UI::Widgets::renderEmptyState(state.unavailableHeading.c_str());
        return ServicesViewContent::Unsupported;
    }
    if (publication == nullptr || publication->version == 0)
    {
        UI::Widgets::renderEmptyState(ICON_FA_GEARS "  Reading services...");
        return ServicesViewContent::Loading;
    }
    if (publication->stale && publication->services.empty())
    {
        // Never read successfully: say why instead of showing an empty table as if there were none.
        UI::Widgets::renderEmptyState(ICON_FA_GEARS "  Couldn't read the services", publication->failureReason.c_str());
        return ServicesViewContent::ReadFailed;
    }

    const auto& scheme = UI::Theme::get().scheme();
    ImGui::SetNextItemWidth(ProcessTableLayout::computeFilterWidth(
        ImGui::CalcTextSize(FILTER_HINT).x, ImGui::GetStyle().FramePadding.x, ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, scheme.textMuted);
    ImGui::InputTextWithHint("##ServiceFilter", FILTER_HINT, &state.filter);
    ImGui::PopStyleColor();
    rebuildRowsIfStale(*publication, state);
    ImGui::SameLine();
    ImGui::TextColored(scheme.textMuted, "%zu of %zu services", state.rows.size(), publication->services.size());
    const ServicesViewContent content = publication->stale ? ServicesViewContent::StaleTable : ServicesViewContent::Table;
    if (publication->stale)
    {
        // The rows are the last good read; the latest one failed.
        ImGui::TextColored(scheme.textMuted, ICON_FA_TRIANGLE_EXCLAMATION "  Out of date: %s", publication->failureReason.c_str());
    }

    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                                            ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                                            ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                            ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##ServicesTable", 6, TABLE_FLAGS))
    {
        return content;
    }
    const float em = ImGui::GetFontSize();
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort, em * 12.0F, static_cast<ImGuiID>(ServiceColumn::Name));
    ImGui::TableSetupColumn("Display name", ImGuiTableColumnFlags_None, em * 20.0F, static_cast<ImGuiID>(ServiceColumn::DisplayName));
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_None, em * 6.0F, static_cast<ImGuiID>(ServiceColumn::State));
    ImGui::TableSetupColumn("Start type", ImGuiTableColumnFlags_None, em * 9.0F, static_cast<ImGuiID>(ServiceColumn::StartType));
    ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_None, em * 4.0F, static_cast<ImGuiID>(ServiceColumn::Pid));
    ImGui::TableSetupColumn("Account", ImGuiTableColumnFlags_None, em * 14.0F, static_cast<ImGuiID>(ServiceColumn::Account));
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsDirty)
    {
        if (specs->SpecsCount > 0)
        {
            state.sortColumn = static_cast<ServiceColumn>(specs->Specs[0].ColumnUserID);
            state.ascending = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
        }
        specs->SpecsDirty = false;
    }

    rebuildRowsIfStale(*publication, state);

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(state.rows.size()));
    while (clipper.Step())
    {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
        {
            const auto& service = publication->services[state.rows[static_cast<std::size_t>(row)]];
            ImGui::TableNextRow();
            ImGui::PushID(row);

            ImGui::TableNextColumn();
            ImGui::Selectable(service.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            {
                renderServiceTooltip(service);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(service.displayName.c_str());
            ImGui::TableNextColumn();
            const std::string_view stateLabel = serviceStateLabel(service.state);
            ImGui::PushStyleColor(ImGuiCol_Text, serviceStateColor(service.state, scheme));
            ImGui::TextUnformatted(stateLabel.data(), stateLabel.data() + stateLabel.size());
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            const std::string_view startLabel = serviceStartTypeLabel(service.startType);
            ImGui::TextUnformatted(startLabel.data(), startLabel.data() + startLabel.size());
            ImGui::TableNextColumn();
            if (service.pid != 0)
            {
                ImGui::Text("%u", service.pid);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(service.account.c_str());

            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    return content;
}

} // namespace App
