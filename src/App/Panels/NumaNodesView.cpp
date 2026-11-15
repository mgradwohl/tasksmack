#include "NumaNodesView.h"

#include "App/Panels/NumaNodesText.h"
#include "Domain/SystemSnapshot.h"
#include "UI/Card.h"
#include "UI/ChromeWidgets.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <cfloat>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace App::NumaNodesView
{

namespace
{

void textView(std::string_view text)
{
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

} // namespace

bool render(const Domain::SystemSnapshot& snap)
{
    if (!NumaNodesText::showNumaNodes(snap))
    {
        return false;
    }
    const auto& theme = UI::Theme::get();
    // Built when the nodes change, not every frame (#3808). UI thread only, as this card is.
    static NumaNodesText::TableCache cache;
    const NumaNodesText::Table& table = cache.get(snap);
    const std::span<const NumaNodesText::Column> columns = table.columns;

    // A card as tall as its table, as the Interface Status card is (#1587).
    if (UI::Widgets::beginCompactCard("##NumaNodesCard", ImVec2(-FLT_MIN, 0.0F), ImGuiChildFlags_AutoResizeY))
    {
        (void) UI::Widgets::sectionHeader(ICON_FA_SITEMAP, table.heading);
        ImGui::Spacing();
        constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##NumaNodesTable", static_cast<int>(columns.size()), TABLE_FLAGS))
        {
            for (const NumaNodesText::Column& column : columns)
            {
                ImGui::TableSetupColumn(column.name, ImGuiTableColumnFlags_None, column.weight);
            }
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (const NumaNodesText::Column& column : columns)
            {
                ImGui::TableNextColumn();
                ImGui::TableHeader(column.name);
                if (!column.tooltip.empty() && ImGui::BeginItemTooltip())
                {
                    textView(column.tooltip);
                    ImGui::EndTooltip();
                }
            }
            for (const std::vector<NumaNodesText::Cell>& cells : table.rows)
            {
                ImGui::TableNextRow();
                for (const NumaNodesText::Cell& cell : cells)
                {
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, cell.available ? theme.scheme().textPrimary : theme.scheme().textMuted);
                    textView(cell.text);
                    ImGui::PopStyleColor();
                    if (!cell.tooltip.empty() && ImGui::BeginItemTooltip())
                    {
                        textView(cell.tooltip);
                        ImGui::EndTooltip();
                    }
                }
            }
            ImGui::EndTable();
        }
    }
    UI::Widgets::endCard();
    ImGui::Spacing();
    return true;
}

} // namespace App::NumaNodesView
