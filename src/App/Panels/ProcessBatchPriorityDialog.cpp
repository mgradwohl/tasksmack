#include "ProcessBatchPriorityDialog.h"

#include "Domain/PriorityConfig.h"
#include "ProcessBatchAction.h"
#include "ProcessDetailsPanel_PriorityHelpers.h"
#include "ProcessPriorityView.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
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
#include <vector>

namespace App
{

namespace
{

// The "###" part keeps the popup's ID fixed while its title names the count.
constexpr const char* POPUP_ID = "###BatchPriority";

constexpr const char* CANCEL_LABEL = "Cancel";

// Floor on the footer's buttons, in ems, as for the other action dialogs (ProcessActionConfirm).
constexpr float BUTTON_MIN_EM = 11.25F;

// The Windows dialog's width, in ems, unless its class slider needs more for its stop names (#1538);
// room for the list's three columns too.
constexpr float WINDOWS_DIALOG_WIDTH_EM = 30.0F;

/// The dialog's width: room for the nice slider at its authored width and its "High" / "Low" labels
/// (on Windows, for the class slider with its stop names) and for the process list, never wider than
/// the viewport allows.
[[nodiscard]] float dialogWidth()
{
    const float emPx = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float contentWidth =
        Detail::PRIORITY_USES_WINDOWS_CLASSES
            ? std::max(WINDOWS_DIALOG_WIDTH_EM * emPx, Detail::discretePrioritySliderMinWidth(Detail::WINDOWS_PRIORITY_SLIDER))
            : (Detail::PRIORITY_SLIDER_WIDTH_EM * emPx) + (2.0F * Detail::PRIORITY_LABEL_PADDING_EM * emPx) +
                  ImGui::CalcTextSize("High").x + ImGui::CalcTextSize("Low").x;
    const float wanted = contentWidth + (style.WindowPadding.x * 2.0F);
    return std::min(wanted, UI::DialogMetrics::computeDialogMaxExtent(ImGui::GetMainViewport()->WorkSize.x));
}

/// The affected processes, a row each (name, PID, current priority) in a bordered list, then "+N more"
/// for the ones folded away: the confirmation the dialog carries itself (#1539).
void renderTargetList(const ProcessBatch::ListedTargets& listed)
{
    const auto& theme = UI::Theme::get();
    const ImGuiStyle& style = ImGui::GetStyle();
    const std::size_t rows = listed.listed.size() + (listed.more > 0 ? 1U : 0U);
    const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
    const float height = (static_cast<float>(rows) * rowHeight) + (style.WindowPadding.y * 2.0F);

    // A card, as the CPU Cores grid draws its cells: bordered, in the theme's child background.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.scheme().childBg);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.scheme().separator);
    if (ImGui::BeginChild("##BatchTargets", ImVec2(-FLT_MIN, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar))
    {
        if (ImGui::BeginTable("##BatchTargetRows", 3, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0F);
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthStretch, 1.0F);
            ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthStretch, 2.0F);
            for (const ProcessBatch::BatchTarget* target : listed.listed)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(target->name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::to_string(target->target.pid).c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(theme.scheme().textMuted, "%s", target->priority.empty() ? "-" : target->priority.c_str());
            }
            if (listed.more > 0)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(theme.scheme().textMuted, "+%zu more", listed.more);
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

} // namespace

void ProcessBatchPriorityDialog::open(std::vector<ProcessBatch::BatchTarget> targets, std::int32_t ownPid)
{
    m_Targets = std::move(targets);
    // Built once here, not every frame the dialog is up: the list points into m_Targets.
    m_Listed = ProcessBatch::listTargets(m_Targets, ownPid);
    m_Warnings.clear();
    ProcessBatch::appendNotableWarnings(m_Warnings, m_Listed.notable, "changed");
    // appendNotableWarnings() writes paragraphs for a confirmation's body; here they stand alone.
    const std::size_t firstText = m_Warnings.find_first_not_of('\n');
    m_Warnings.erase(0, firstText == std::string::npos ? m_Warnings.size() : firstText);
    m_ApplyLabel = std::format("Apply to {}", ProcessBatch::processCountText(m_Targets.size()));
    m_Nice = Domain::Priority::NORMAL_NICE;
    m_ShowRequested = true;
}

std::optional<std::int32_t> ProcessBatchPriorityDialog::render()
{
    if (m_ShowRequested)
    {
        ImGui::OpenPopup(POPUP_ID);
        m_ShowRequested = false;
    }
    m_Open = ImGui::IsPopupOpen(POPUP_ID);
    if (!m_Open)
    {
        return std::nullopt; // Nothing to draw; skip building the title every frame
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const std::string title =
        std::format("{}  Set priority for {}{}", ICON_FA_GAUGE_HIGH, ProcessBatch::processCountText(m_Targets.size()), POPUP_ID);
    // Centred over the main window (#1539). A fixed width, so the slider is laid out at its own width
    // from the first frame, and a height fitted to the contents (0); both capped at the viewport, and
    // the dialog kept inside it.
    UI::Widgets::centerNextDialog(m_LastWorkSize);
    ImGui::SetNextWindowSize(ImVec2(dialogWidth(), 0.0F));
    UI::Widgets::setNextDialogSizeConstraints(ImVec2(UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x),
                                                     UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.y)));
    std::optional<std::int32_t> chosen;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        return chosen;
    }
    UI::Widgets::keepCurrentWindowInViewport();
    const auto& theme = UI::Theme::get();

    // What will happen: Process Details' own control, so the batch is picked exactly as one process is
    // (#1484), then the value it applies.
    pickNice(Detail::renderPriorityPicker(m_Nice).nice);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(std::format("Applies: {}", ProcessBatch::priorityValueText(m_Nice)).c_str());
    if (!Detail::PRIORITY_USES_WINDOWS_CLASSES && m_Nice < Domain::Priority::NORMAL_NICE)
    {
        // Raising the priority needs privileges on Linux: say so before the batch, not only in its result.
        ImGui::TextColored(theme.scheme().textWarning,
                           ICON_FA_TRIANGLE_EXCLAMATION "  A nice value below 0 usually needs root; without it, each process will refuse.");
    }
    ImGui::PopTextWrapPos();

    // To which processes: listed here, so Apply is the confirmation (no second dialog, #1539).
    ImGui::Spacing();
    (void) UI::Widgets::sectionHeader(ICON_FA_LIST, "Processes");
    renderTargetList(m_Listed);
    if (!m_Warnings.empty())
    {
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextColored(theme.scheme().textWarning, "%s", m_Warnings.c_str());
        ImGui::PopTextWrapPos();
    }

    // Apply is the primary action: the success fill Settings' Save uses (#1539), Cancel the plain one.
    const UI::Widgets::ButtonFills applyFills{
        .resting = theme.scheme().successButton,
        .hovered = theme.scheme().successButtonHovered,
        .pressed = theme.scheme().successButtonActive,
    };
    const UI::Widgets::DialogFooterButton applyButton{.label = m_ApplyLabel.c_str(), .fills = &applyFills, .tooltip = nullptr};
    const UI::Widgets::DialogFooterButton cancelButton{.label = CANCEL_LABEL, .fills = nullptr, .tooltip = nullptr};
    switch (UI::Widgets::dialogFooter(
        applyButton, cancelButton, UI::Widgets::footerButtonWidth({m_ApplyLabel.c_str(), CANCEL_LABEL}, BUTTON_MIN_EM)))
    {
    case UI::Widgets::DialogFooterAction::Primary:
        chosen = m_Nice;
        m_Open = false;
        ImGui::CloseCurrentPopup();
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
    return chosen;
}

} // namespace App
