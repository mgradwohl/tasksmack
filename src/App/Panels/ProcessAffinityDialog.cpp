#include "ProcessAffinityDialog.h"

#include "App/Panels/ProcessAffinitySections.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/Format.h"
#include "UI/FrameOutline.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>
#include <imgui_internal.h> // ImGuiItemFlags_MixedValue: a section heading's box while only some of its CPUs are ticked

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
// Shown in place of the boxes, and as Apply's tooltip, until the CPU layout has been read (#3182).
constexpr const char* READING_LAYOUT_TEXT = "Reading this PC's CPU layout...";

// Floor on the footer's buttons, in ems, as for the other action dialogs (ProcessActionConfirm).
constexpr float BUTTON_MIN_EM = 11.25F;
// The dialog's width in ems: room for eight CPU boxes a row at four-digit CPU numbers.
constexpr float DIALOG_WIDTH_EM = 34.0F;
// The CPU grid's height cap in rows; more scroll (a 256-CPU server).
constexpr float GRID_MAX_ROWS = 12.0F;

} // namespace

void ProcessAffinityDialog::open(Platform::ProcessTarget target,
                                 std::string name,
                                 const Platform::CpuAffinity& current,
                                 std::size_t cpuCount,
                                 const Platform::CpuAffinityLayout& layout)
{
    m_Target = target;
    m_Name = std::move(name);
    m_AwaitingLayout = false;
    m_AwaitedCurrent = {};
    arrange(current, cpuCount, layout);
    m_ShowRequested = true;
}

void ProcessAffinityDialog::openAwaitingLayout(Platform::ProcessTarget target,
                                               std::string name,
                                               const Platform::CpuAffinity& current,
                                               std::size_t cpuCount)
{
    m_Target = target;
    m_Name = std::move(name);
    m_AwaitingLayout = true;
    m_AwaitedCurrent = current;
    m_AwaitedCpuCount = cpuCount;
    // No boxes, so nothing to apply, until the layout arrives.
    m_Chosen.clear();
    m_Offered.clear();
    m_OfferedCount = 0;
    m_Sections.clear();
    m_GroupCount = 0;
    m_AppliedAsCpuSets = false;
    m_ShowRequested = true;
}

void ProcessAffinityDialog::setLayout(const Platform::CpuAffinityLayout& layout)
{
    if (!m_AwaitingLayout)
    {
        return;
    }
    m_AwaitingLayout = false;
    arrange(m_AwaitedCurrent, m_AwaitedCpuCount, layout);
    m_AwaitedCurrent = {};
}

void ProcessAffinityDialog::arrange(const Platform::CpuAffinity& current, std::size_t cpuCount, const Platform::CpuAffinityLayout& layout)
{
    const std::vector<std::size_t> offered = affinityDialogOfferedCpus(current, cpuCount, layout);
    m_Chosen.assign(offered.back() + 1, 0); // offered is ascending and never empty
    m_Offered.assign(m_Chosen.size(), 0);
    for (const std::size_t cpu : offered)
    {
        m_Offered[cpu] = 1;
        // An affinity that couldn't be read starts with every box ticked: the process can run somewhere.
        m_Chosen[cpu] = (current.empty() || current.test(cpu)) ? 1 : 0;
    }
    m_OfferedCount = offered.size();
    m_Sections = affinityDialogSections(offered, layout);
    m_GroupCount = layout.groupSizes.size();
    m_AppliedAsCpuSets = layout.appliedAsCpuSets;
}

void ProcessAffinityDialog::choose(std::size_t cpu, bool chosen) noexcept
{
    if (cpu < m_Chosen.size() && m_Offered[cpu] != 0)
    {
        m_Chosen[cpu] = chosen ? 1 : 0;
    }
}

void ProcessAffinityDialog::chooseAll(bool chosen) noexcept
{
    std::size_t cpu = 0;
    while (cpu < m_Chosen.size())
    {
        m_Chosen[cpu] = (chosen && m_Offered[cpu] != 0) ? 1 : 0;
        ++cpu;
    }
}

void ProcessAffinityDialog::chooseSection(std::size_t index, bool chosen) noexcept
{
    if (index < m_Sections.size())
    {
        for (const std::size_t cpu : m_Sections[index].cpus)
        {
            choose(cpu, chosen);
        }
    }
}

bool ProcessAffinityDialog::canApply() const noexcept
{
    return std::ranges::any_of(m_Chosen, [](std::uint8_t ticked) { return ticked != 0; });
}

Platform::CpuAffinity ProcessAffinityDialog::choice() const
{
    Platform::CpuAffinity cpus;
    std::size_t cpu = 0;
    while (cpu < m_Chosen.size())
    {
        if (m_Chosen[cpu] != 0)
        {
            cpus.set(cpu);
        }
        ++cpu;
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
    // Escape cancels, as Cancel does: the affinity is left alone (#1944).
    const bool escaped = UI::Widgets::dialogEscapePressed();
    const auto& theme = UI::Theme::get();

    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted("The CPUs this process may run on. Every thread is changed, and threads it starts later inherit it.");
    if (m_AppliedAsCpuSets)
    {
        // One affinity mask can't span processor groups, so Windows gets the choice as CPU sets (#1881).
        ImGui::TextColored(theme.scheme().textMuted,
                           "This PC has %zu processor groups, so Windows sets the choice as the process's default CPU sets: threads "
                           "the process has given CPUs of their own keep them, and the Affinity column keeps showing its affinity mask.",
                           m_GroupCount);
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (m_AwaitingLayout)
    {
        // The CPU layout is read once, on a worker (#3182): the frame or two before it has arrived.
        ImGui::TextColored(theme.scheme().textMuted, "%s", READING_LAYOUT_TEXT);
    }
    else
    {
        renderChoices();
    }

    const bool anyChosen = canApply();
    const UI::Widgets::ButtonFills applyFills{
        .resting = theme.scheme().successButton,
        .hovered = theme.scheme().successButtonHovered,
        .pressed = theme.scheme().successButtonActive,
    };
    // Apply is disabled while nothing is ticked (#1881), or there are no boxes yet; the line above says why.
    const char* applyTooltip = anyChosen ? nullptr : "Choose at least one CPU";
    if (m_AwaitingLayout)
    {
        applyTooltip = READING_LAYOUT_TEXT;
    }
    const UI::Widgets::DialogFooterButton applyButton{
        .label = APPLY_LABEL, .fills = &applyFills, .tooltip = applyTooltip, .disabled = !anyChosen};
    const UI::Widgets::DialogFooterButton cancelButton{.label = CANCEL_LABEL, .fills = nullptr, .tooltip = nullptr};
    UI::Widgets::DialogFooterAction pressed =
        UI::Widgets::dialogFooter(applyButton, cancelButton, UI::Widgets::footerButtonWidth({APPLY_LABEL, CANCEL_LABEL}, BUTTON_MIN_EM));
    if (escaped)
    {
        pressed = UI::Widgets::DialogFooterAction::Secondary;
    }
    switch (pressed)
    {
    case UI::Widgets::DialogFooterAction::Primary:
        if (anyChosen)
        {
            applied = choice();
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

void ProcessAffinityDialog::renderChoices()
{
    const auto& theme = UI::Theme::get();
    if (ImGui::SmallButton(ALL_LABEL))
    {
        chooseAll(true);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(NONE_LABEL))
    {
        chooseAll(false);
    }

    // The boxes in a grid per section, numbered as the CPU Cores section numbers them, in a bordered
    // scrolling list. A titled section's heading is a box of its own, mixed while only some are ticked.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float boxWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("0000").x + style.ItemSpacing.x;
    const auto columns = static_cast<std::size_t>(std::max(1.0F, ImGui::GetContentRegionAvail().x / boxWidth));
    std::size_t rowCount = 0;
    for (const AffinityDialogSection& section : m_Sections)
    {
        rowCount += ((section.cpus.size() + columns - 1) / columns) + (section.title.empty() ? 0U : 1U); // rounded up, plus its heading
    }
    const float gridHeight =
        (std::min(static_cast<float>(rowCount), GRID_MAX_ROWS) * ImGui::GetFrameHeightWithSpacing()) + (style.WindowPadding.y * 2.0F);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.scheme().childBg);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.scheme().separator);
    if (ImGui::BeginChild("##AffinityCpus", ImVec2(-FLT_MIN, gridHeight), ImGuiChildFlags_Borders))
    {
        std::size_t index = 0;
        while (index < m_Sections.size())
        {
            const AffinityDialogSection& section = m_Sections[index];
            ImGui::PushID(static_cast<int>(index));
            if (!section.title.empty())
            {
                const auto ticked =
                    static_cast<std::size_t>(std::ranges::count_if(section.cpus, [this](std::size_t cpu) { return m_Chosen[cpu] != 0; }));
                bool all = ticked == section.cpus.size();
                ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, ticked > 0 && !all);
                const std::string heading = std::format("{} ({})###Section", section.title, section.cpus.size());
                if (UI::Widgets::outlinedCheckbox(heading.c_str(), &all))
                {
                    chooseSection(index, all);
                }
                ImGui::PopItemFlag();
            }
            if (ImGui::BeginTable("##AffinityGrid", static_cast<int>(columns), ImGuiTableFlags_SizingStretchSame))
            {
                for (const std::size_t cpu : section.cpus)
                {
                    ImGui::TableNextColumn();
                    bool ticked = m_Chosen[cpu] != 0;
                    const std::string label = std::to_string(cpu);
                    if (UI::Widgets::outlinedCheckbox(label.c_str(), &ticked))
                    {
                        choose(cpu, ticked);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
            ++index;
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
        ImGui::TextColored(theme.scheme().textMuted, "%zu of %zu CPUs", chosen.count(), m_OfferedCount);
    }
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
    std::string text = std::format("CPU affinity of {} (PID {}) set to {}", name, pid, UI::Format::formatCpuAffinity(cpus.words()));
    if (!result.successNote.empty())
    {
        // How the platform applied it, when that is weaker than a hard affinity (#2173)
        text += ' ';
        text += result.successNote;
    }
    return {.ok = true, .text = std::move(text)};
}

} // namespace App
