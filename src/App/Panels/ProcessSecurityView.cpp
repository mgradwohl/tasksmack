#include "ProcessSecurityView.h"

#include "Platform/IProcessSecurity.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <string>

namespace App
{

void ProcessSecurityView::render(bool hasSecurity)
{
    if (!hasSecurity)
    {
        return; // Windows for now, synthetic runs: no section at all
    }
    // Collapsed by default: nothing is read until it is opened.
    if (!ImGui::CollapsingHeader(ICON_FA_LOCK "  Security###ProcessSecurity"))
    {
        return;
    }
    markDrawnOpen();

    const auto& scheme = UI::Theme::get().scheme();
    if (!m_HasRead)
    {
        ImGui::TextColored(scheme.textMuted, "Reading...");
        return;
    }
    if (m_Status != Platform::SecurityReadStatus::Ok)
    {
        // A muted note, not an empty table and not an error: protected processes refuse this routinely.
        std::string text(Detail::securityStatusText(m_Status));
        if (m_Status == Platform::SecurityReadStatus::Failed && !m_Detail.empty())
        {
            text += ": " + m_Detail;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
        return;
    }
    if (m_Rows.empty())
    {
        ImGui::TextColored(scheme.textMuted, "Nothing reported");
        return;
    }

    // Label and value; a value too long for its column (a capability list, a cgroup path) wraps rather
    // than running off the pane.
    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##SecurityTable", 2, TABLE_FLAGS))
    {
        return;
    }
    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    for (const Detail::SecurityRow& row : m_Rows)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(scheme.textMuted, "%s", row.label.c_str());
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", row.value.c_str());
    }
    ImGui::EndTable();
}

} // namespace App
