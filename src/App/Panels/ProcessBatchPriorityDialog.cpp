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
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace App
{

namespace
{

// The "###" part keeps the popup's ID fixed while its title names the count.
constexpr const char* POPUP_ID = "###BatchPriority";

constexpr const char* CONTINUE_LABEL = "Continue";

// Floor on the footer's buttons, in ems, as for the confirmation that follows (ProcessActionConfirm).
constexpr float BUTTON_MIN_EM = 11.25F;

// The Windows dialog's width, in ems: room for its sentence beside the class combo.
constexpr float WINDOWS_DIALOG_WIDTH_EM = 30.0F;

/// The dialog's width: room for the nice slider at its authored width and its "High" / "Low" labels
/// (on Windows, for the class combo and its sentence), never wider than the viewport allows.
[[nodiscard]] float dialogWidth()
{
    const float emPx = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float contentWidth = Detail::PRIORITY_USES_WINDOWS_CLASSES
                                 ? WINDOWS_DIALOG_WIDTH_EM * emPx
                                 : (Detail::PRIORITY_SLIDER_WIDTH_EM * emPx) + (2.0F * Detail::PRIORITY_LABEL_PADDING_EM * emPx) +
                                       ImGui::CalcTextSize("High").x + ImGui::CalcTextSize("Low").x;
    const float wanted = contentWidth + (style.WindowPadding.x * 2.0F);
    return std::min(wanted, UI::DialogMetrics::computeDialogMaxExtent(ImGui::GetMainViewport()->WorkSize.x));
}

} // namespace

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
    const std::string title = std::format("Set priority for {}{}", ProcessBatch::processCountText(m_Count), POPUP_ID);
    // A fixed width, so the slider is laid out at its own width from the first frame, and a height
    // fitted to the contents (0); both capped at the viewport, and the dialog kept inside it.
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

    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(Detail::PRIORITY_USES_WINDOWS_CLASSES ? "Pick the priority class for every selected process."
                                                                 : "Pick the nice value for every selected process.");
    ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textMuted);
    ImGui::TextUnformatted("The processes are listed for you to confirm next.");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Spacing();

    // Process Details' own control, so the batch is picked exactly as one process is (#1484).
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

    const UI::Widgets::DialogFooterButton continueButton{.label = CONTINUE_LABEL, .fills = nullptr, .tooltip = nullptr};
    const UI::Widgets::DialogFooterButton cancelButton{.label = "Cancel", .fills = nullptr, .tooltip = nullptr};
    switch (
        UI::Widgets::dialogFooter(continueButton, cancelButton, UI::Widgets::footerButtonWidth({CONTINUE_LABEL, "Cancel"}, BUTTON_MIN_EM)))
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
