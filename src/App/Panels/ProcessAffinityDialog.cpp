#include "ProcessAffinityDialog.h"

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace App
{

namespace
{

// The "###" part keeps the popup's ID fixed while its title names the process.
constexpr const char* POPUP_ID = "###SetAffinity";
constexpr const char* APPLY_LABEL = "Apply";
constexpr const char* CANCEL_LABEL = "Cancel";
constexpr const char* ALL_LABEL = "All";
constexpr const char* NONE_LABEL = "None";

// Floor on the footer's buttons, in ems, as for the other action dialogs (ProcessActionConfirm).
constexpr float BUTTON_MIN_EM = 11.25F;
// The dialog's width in ems: room for eight CPU boxes a row at four-digit CPU numbers.
constexpr float DIALOG_WIDTH_EM = 34.0F;
// The CPU grid's height cap in rows; more scroll (a 256-CPU server).
constexpr float GRID_MAX_ROWS = 12.0F;

} // namespace

std::size_t affinityDialogCpuCount(const Platform::CpuAffinity& current, std::size_t onlineCpus) noexcept
{
    std::size_t highest = 0;
    const std::size_t width = current.words().size() * Platform::CpuAffinity::BITS_PER_WORD;
    for (std::size_t cpu = width; cpu > 0; --cpu)
    {
        if (current.test(cpu - 1))
        {
            highest = cpu;
            break;
        }
    }
    return std::max({onlineCpus, highest, std::size_t{1}});
}

void ProcessAffinityDialog::open(Platform::ProcessTarget target,
                                 std::string name,
                                 const Platform::CpuAffinity& current,
                                 std::size_t cpuCount)
{
    m_Target = target;
    m_Name = std::move(name);
    m_Chosen.assign(affinityDialogCpuCount(current, cpuCount), 0);
    for (std::size_t cpu = 0; cpu < m_Chosen.size(); ++cpu)
    {
        // An affinity that couldn't be read starts with every box ticked: the process can run somewhere.
        m_Chosen[cpu] = (current.empty() || current.test(cpu)) ? 1 : 0;
    }
    m_ShowRequested = true;
}

void ProcessAffinityDialog::choose(std::size_t cpu, bool chosen) noexcept
{
    if (cpu < m_Chosen.size())
    {
        m_Chosen[cpu] = chosen ? 1 : 0;
    }
}

void ProcessAffinityDialog::chooseAll(bool chosen) noexcept
{
    std::ranges::fill(m_Chosen, chosen ? std::uint8_t{1} : std::uint8_t{0});
}

Platform::CpuAffinity ProcessAffinityDialog::choice() const
{
    Platform::CpuAffinity cpus;
    for (std::size_t cpu = 0; cpu < m_Chosen.size(); ++cpu)
    {
        if (m_Chosen[cpu] != 0)
        {
            cpus.set(cpu);
        }
    }
    return cpus;
}

std::optional<Platform::CpuAffinity> ProcessAffinityDialog::render()
{
    if (m_ShowRequested)
    {
        ImGui::OpenPopup(POPUP_ID);
        m_ShowRequested = false;
    }
    m_Open = ImGui::IsPopupOpen(POPUP_ID);
    if (!m_Open)
    {
        return std::nullopt;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const std::string title = std::format("{}  Set affinity for {} (PID {}){}", ICON_FA_MICROCHIP, m_Name, m_Target.pid, POPUP_ID);
    UI::Widgets::centerNextDialog(m_LastWorkSize);
    const float width = std::min((DIALOG_WIDTH_EM * ImGui::GetFontSize()) + (ImGui::GetStyle().WindowPadding.x * 2.0F),
                                 UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0F));
    UI::Widgets::setNextDialogSizeConstraints(ImVec2(UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x),
                                                     UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.y)));
    std::optional<Platform::CpuAffinity> applied;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        return applied;
    }
    UI::Widgets::keepCurrentWindowInViewport();
    if (std::exchange(m_DismissRequested, false))
    {
        m_Open = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return applied;
    }
    const auto& theme = UI::Theme::get();

    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted("The CPUs this process may run on. Every thread is changed, and threads it starts later inherit it.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (ImGui::SmallButton(ALL_LABEL))
    {
        chooseAll(true);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(NONE_LABEL))
    {
        chooseAll(false);
    }

    // The boxes in a grid, numbered as the CPU Cores section numbers them, in a bordered scrolling list.
    const float boxWidth =
        ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("0000").x + ImGui::GetStyle().ItemSpacing.x;
    const auto columns = static_cast<int>(std::max(1.0F, ImGui::GetContentRegionAvail().x / boxWidth));
    const std::size_t rowCount =
        (m_Chosen.size() + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns); // rounded up
    const auto rows = static_cast<float>(rowCount);
    const float gridHeight =
        (std::min(rows, GRID_MAX_ROWS) * ImGui::GetFrameHeightWithSpacing()) + (ImGui::GetStyle().WindowPadding.y * 2.0F);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.scheme().childBg);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.scheme().separator);
    if (ImGui::BeginChild("##AffinityCpus", ImVec2(-FLT_MIN, gridHeight), ImGuiChildFlags_Borders))
    {
        if (ImGui::BeginTable("##AffinityGrid", columns, ImGuiTableFlags_SizingStretchSame))
        {
            for (std::size_t cpu = 0; cpu < m_Chosen.size(); ++cpu)
            {
                ImGui::TableNextColumn();
                bool ticked = m_Chosen[cpu] != 0;
                const std::string label = std::to_string(cpu);
                if (ImGui::Checkbox(label.c_str(), &ticked))
                {
                    choose(cpu, ticked);
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    const Platform::CpuAffinity chosen = choice();
    const bool anyChosen = !chosen.empty();
    if (!anyChosen)
    {
        ImGui::TextColored(theme.scheme().textWarning, ICON_FA_TRIANGLE_EXCLAMATION "  Choose at least one CPU.");
    }
    else
    {
        ImGui::TextColored(theme.scheme().textMuted, "%zu of %zu CPUs", chosen.count(), m_Chosen.size());
    }

    const UI::Widgets::ButtonFills applyFills{
        .resting = theme.scheme().successButton,
        .hovered = theme.scheme().successButtonHovered,
        .pressed = theme.scheme().successButtonActive,
    };
    // Apply with nothing chosen does nothing: the line above says why (the footer can't disable one button).
    const UI::Widgets::DialogFooterButton applyButton{.label = APPLY_LABEL, .fills = &applyFills, .tooltip = nullptr};
    const UI::Widgets::DialogFooterButton cancelButton{.label = CANCEL_LABEL, .fills = nullptr, .tooltip = nullptr};
    switch (
        UI::Widgets::dialogFooter(applyButton, cancelButton, UI::Widgets::footerButtonWidth({APPLY_LABEL, CANCEL_LABEL}, BUTTON_MIN_EM)))
    {
    case UI::Widgets::DialogFooterAction::Primary:
        if (anyChosen)
        {
            applied = chosen;
            m_Open = false;
            ImGui::CloseCurrentPopup();
        }
        break;
    case UI::Widgets::DialogFooterAction::Secondary:
        m_Open = false;
        ImGui::CloseCurrentPopup();
        break;
    case UI::Widgets::DialogFooterAction::None:
    case UI::Widgets::DialogFooterAction::Leading:
        break;
    }
    ImGui::EndPopup();
    return applied;
}

Detail::ActionResultMessage affinityResultMessage(const std::string& name,
                                                  std::int32_t pid,
                                                  const Platform::CpuAffinity& cpus,
                                                  const Platform::ProcessActionResult& result)
{
    if (!result.success)
    {
        return {.ok = false, .text = result.errorMessage};
    }
    return {.ok = true,
            .text = std::format("CPU affinity of {} (PID {}) set to {}", name, pid, UI::Format::formatCpuAffinity(cpus.words()))};
}

} // namespace App
