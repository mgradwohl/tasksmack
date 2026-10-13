#include "ProcessServicesView.h"

#include "App/Panels/ServicesView.h"
#include "Domain/ServiceModel.h"
#include "Platform/IServiceProbe.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{

namespace Detail
{

std::vector<HostedServiceRow> hostedServiceRows(std::span<const std::string> names, const Domain::ServicePublication* publication)
{
    std::vector<HostedServiceRow> rows;
    rows.reserve(names.size());
    std::size_t index = 0;
    while (index < names.size())
    {
        HostedServiceRow row{.name = names[index], .displayName = {}, .state = Platform::ServiceState::Unknown, .listed = false};
        if (const Platform::ServiceInfo* service = (publication != nullptr) ? Domain::findService(*publication, names[index]) : nullptr;
            service != nullptr)
        {
            row.displayName = service->displayName;
            row.state = service->state;
            row.listed = true;
        }
        rows.push_back(std::move(row));
        ++index;
    }
    return rows;
}

} // namespace Detail

void ProcessServicesView::rebuildRowsIfStale(std::span<const std::string> names, const Domain::ServicePublication* publication)
{
    const std::uint64_t version = (publication != nullptr) ? publication->version : 0;
    if (version == m_RowsVersion && std::ranges::equal(names, m_RowsNames))
    {
        return;
    }
    m_Rows = Detail::hostedServiceRows(names, publication);
    m_RowsNames.assign(names.begin(), names.end());
    m_RowsVersion = version;
}

std::optional<std::string> ProcessServicesView::render(bool hasServices, std::span<const std::string> names, const ServiceList& list)
{
    if (!hasServices)
    {
        return std::nullopt; // this platform cannot tell: no section at all
    }
    // Collapsed by default: the service list is kept current only while it is open.
    if (!ImGui::CollapsingHeader(ICON_FA_GEARS "  Services###ProcessServices"))
    {
        return std::nullopt;
    }
    m_DrawnOpen = true;

    const auto& scheme = UI::Theme::get().scheme();
    if (names.empty())
    {
        ImGui::TextColored(scheme.textMuted, "%s", (list.expected && list.publication == nullptr) ? "Reading services..." : "No services");
        return std::nullopt;
    }
    rebuildRowsIfStale(names, list.publication);

    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##ServicesTable", 3, TABLE_FLAGS))
    {
        return std::nullopt;
    }
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Display name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();

    std::optional<std::string> clicked;
    std::size_t index = 0;
    while (index < m_Rows.size())
    {
        const Detail::HostedServiceRow& row = m_Rows[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (row.listed)
        {
            // Opens the Services tab with this service selected (#1880).
            if (ImGui::TextLink(row.name.c_str()))
            {
                clicked = row.name;
            }
            ImGui::SetItemTooltip("Show %s in the Services tab", row.name.c_str());
        }
        else
        {
            ImGui::TextUnformatted(row.name.c_str());
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.displayName.c_str());
        ImGui::TableNextColumn();
        if (row.listed)
        {
            const std::string_view label = serviceStateLabel(row.state);
            ImGui::PushStyleColor(ImGuiCol_Text, serviceStateColor(row.state, scheme));
            ImGui::TextUnformatted(label.data(), label.data() + label.size());
            ImGui::PopStyleColor();
        }
        else
        {
            // Not in the service list (Linux, until systemd's is read): its state is not known here.
            ImGui::TextColored(scheme.textMuted, "\xE2\x80\x94");
        }
        ImGui::PopID();
        ++index;
    }
    ImGui::EndTable();
    return clicked;
}

} // namespace App
