#include "ProcessActionConfirm.h"

#include "ProcessDetailsLayout.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "UI/ChromeLayout.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace App::ProcessActionConfirm
{

namespace
{
// Floor on the dialog's two buttons, in ems: 120px at the reference em.
constexpr float CONFIRM_BUTTON_MIN_EM = 11.25F;

// The title names the action and the process, "Kill firefox (PID 1234)?"; "###" keeps the popup's
// ID fixed while the visible title changes with them (#1203).
constexpr const char* CONFIRM_POPUP_ID = "###ConfirmAction";
} // namespace

UI::Widgets::ButtonFills dangerButtonFills()
{
    const auto& scheme = UI::Theme::get().scheme();
    return {.resting = scheme.dangerButton, .hovered = scheme.dangerButtonHovered, .pressed = scheme.dangerButtonActive};
}

namespace
{

/// Closes the modal unconfirmed if it is open; see render()'s `dismiss`.
Outcome dismissModal(bool& showRequested)
{
    showRequested = false;
    if (!ImGui::IsPopupOpen(CONFIRM_POPUP_ID))
    {
        return Outcome::None;
    }
    // CloseCurrentPopup() acts on the popup being drawn, so the modal is entered to close it. Its
    // ID comes from the "###" part alone, so the bare ID names the same popup; nothing is drawn
    // in it, and no button can be pressed on the way out.
    if (ImGui::BeginPopupModal(CONFIRM_POPUP_ID, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    return Outcome::Cancelled;
}

/// Opens the modal on the frames it is requested; whether it is open, and so must be drawn, now.
bool openIfRequested(bool showRequested)
{
    if (showRequested)
    {
        ImGui::OpenPopup(CONFIRM_POPUP_ID);
    }
    return ImGui::IsPopupOpen(CONFIRM_POPUP_ID);
}

/// Draws the open modal with @p title and @p question.
Outcome renderOpen(bool& showRequested, Detail::ProcessAction action, std::string_view title, std::string_view question)
{
    Outcome outcome = Outcome::None;
    std::string popupTitle(title);
    popupTitle += CONFIRM_POPUP_ID;
    // Never taller or wider than the viewport: a batch question lists up to eight processes plus
    // warnings, so at a large font or in a short window the auto-fitting dialog could otherwise push
    // its buttons off-screen (#804 review). The question scrolls instead (below).
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float dialogMaxHeight = UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.y);
    UI::Widgets::setNextDialogSizeConstraints(ImVec2(UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x), dialogMaxHeight));
    if (ImGui::BeginPopupModal(popupTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        UI::Widgets::keepCurrentWindowInViewport();
        // The dialog auto-fits, so it is bounded here: neither the question (which carries the
        // process name) nor the button row may be wider than the main window can show. See
        // ProcessDetailsLayout::computeConfirmContentBudget().
        const ImGuiStyle& confirmStyle = ImGui::GetStyle();
        const float contentBudget = ProcessDetailsLayout::computeConfirmContentBudget(
            ImGui::GetMainViewport()->WorkSize.x, UI::DialogMetrics::MAX_VIEWPORT_FRACTION, confirmStyle.WindowPadding.x);

        // States what the action does; the title can be cut short by a long name, so the body
        // names the process too (#1203). A batch's question lists its processes a line each (#804).
        // Wrapped at the budget, or at the text's own width when that is narrower -- a wrap
        // position wider than the text would make the auto-fitting dialog as wide as the budget.
        // A multi-line question measures as its widest line.
        // Begin and end pointers: the view need not be NUL-terminated, and ImGui reads up to the end.
        const char* questionBegin = question.data();
        const char* questionEnd = questionBegin + question.size();
        const float questionWidth = ImGui::CalcTextSize(questionBegin, questionEnd).x;
        const float wrapWidth = (contentBudget > 0.0F) ? std::min(questionWidth, contentBudget) : questionWidth;
        // The question takes what the title bar, padding and footer leave; beyond that it scrolls in
        // a child of that height, so Cancel and the action stay reachable (#804 review).
        const float questionHeight = ImGui::CalcTextSize(questionBegin, questionEnd, false, wrapWidth).y;
        const float reservedHeight = ImGui::GetFrameHeight() + (confirmStyle.WindowPadding.y * 2.0F) + confirmStyle.ItemSpacing.y +
                                     UI::ChromeLayout::dialogFooterHeight(confirmStyle.ItemSpacing.y, ImGui::GetFrameHeight(), false);
        const float questionMaxHeight = UI::DialogMetrics::computeScrollableBodyMaxHeight(
            dialogMaxHeight, reservedHeight, ImGui::GetTextLineHeightWithSpacing() * 2.0F);
        const bool scrolls = questionHeight > questionMaxHeight;
        if (scrolls)
        {
            ImGui::BeginChild("##ConfirmQuestion", ImVec2(wrapWidth + confirmStyle.ScrollbarSize, questionMaxHeight));
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
        ImGui::TextUnformatted(questionBegin, questionEnd);
        ImGui::PopTextWrapPos();
        if (scrolls)
        {
            ImGui::EndChild();
        }

        // One width for both, from the font: 11.25 em is the former fixed 120px at the reference
        // em, so the dialog is unchanged there and the buttons stay a comfortable target for a
        // destructive confirmation at any font size or display density (#971).
        //
        // Held to half the dialog's budget: at Even Huger on a 175% display each button wants
        // 420px, and the pair would be wider than a minimum-width window.
        //
        // The confirm button is named for the action ([Cancel][Kill], not [No][Yes]) so a
        // destructive confirmation says what it does on the button itself (#1203). It is the primary
        // action, so it sits on the right in the footer every dialog shares (#1200).
        const char* confirmLabel = Detail::actionLabel(action);
        const float confirmButtonWidth = ProcessDetailsLayout::computeConfirmButtonWidth(
            UI::Widgets::footerButtonWidth({confirmLabel, "Cancel"}, CONFIRM_BUTTON_MIN_EM), contentBudget, confirmStyle.ItemSpacing.x);

        // Ending a process can lose its work, so Terminate and Kill confirm in the danger colour
        // their buttons in the Actions tab use (#1273).
        const UI::Widgets::ButtonFills dangerFills = dangerButtonFills();
        const UI::Widgets::DialogFooterButton confirmButton{
            .label = confirmLabel, .fills = Detail::isDestructiveAction(action) ? &dangerFills : nullptr, .tooltip = nullptr};
        const UI::Widgets::DialogFooterButton cancelButton{.label = "Cancel", .fills = nullptr, .tooltip = nullptr};
        switch (UI::Widgets::dialogFooter(confirmButton, cancelButton, confirmButtonWidth, {}, contentBudget))
        {
        case UI::Widgets::DialogFooterAction::Primary:
            outcome = Outcome::Confirmed;
            showRequested = false;
            ImGui::CloseCurrentPopup();
            break;
        case UI::Widgets::DialogFooterAction::Secondary:
            outcome = Outcome::Cancelled;
            showRequested = false;
            ImGui::CloseCurrentPopup();
            break;
        case UI::Widgets::DialogFooterAction::None:
        case UI::Widgets::DialogFooterAction::Leading:
            break;
        }

        ImGui::EndPopup();
    }
    return outcome;
}

} // namespace

Outcome render(bool& showRequested, Detail::ProcessAction action, std::string_view processName, std::int32_t pid, bool dismiss)
{
    if (dismiss)
    {
        return dismissModal(showRequested);
    }
    if (!openIfRequested(showRequested))
    {
        return Outcome::None; // Nothing to draw; skip building the title every frame
    }
    return renderOpen(showRequested, action, Detail::confirmTitle(action, processName, pid), Detail::confirmBody(action, processName, pid));
}

Outcome renderText(bool& showRequested, Detail::ProcessAction action, std::string_view title, std::string_view question, bool dismiss)
{
    if (dismiss)
    {
        return dismissModal(showRequested);
    }
    if (!openIfRequested(showRequested))
    {
        return Outcome::None;
    }
    return renderOpen(showRequested, action, title, question);
}

} // namespace App::ProcessActionConfirm
