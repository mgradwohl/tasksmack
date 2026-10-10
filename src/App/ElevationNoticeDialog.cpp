#include "ElevationNoticeDialog.h"

#include "App/DialogGeometry.h"
#include "App/ElevationNoticeText.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <ranges>
#include <string_view>

namespace App::ElevationNoticeDialog
{

float measureContentWidth()
{
    // Everything the dialog shows on one line, unwrapped: its widest is the width the text needs.
    const ImGuiStyle& style = ImGui::GetStyle();
    float widest = ImGui::CalcTextSize(ICON_FA_LOCK "  Limited Data").x;
    constexpr std::string_view bodyText = ElevationNoticeText::forCurrentPlatform();
    for (const auto paragraph : std::views::split(bodyText, '\n'))
    {
        const std::string_view line(paragraph.begin(), paragraph.end());
        widest = std::max(widest, ImGui::CalcTextSize(line.data(), line.data() + line.size()).x);
    }
    widest = std::max(widest, ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Don't show again").x);
    widest = std::max(
        widest, UI::DialogMetrics::computeActionButtonWidth(ImGui::CalcTextSize("OK").x, ImGui::GetFontSize(), ELEVATION_BUTTON_MIN_EM));
    // The title bar holds the popup's name between its frame padding rather than the window padding.
    const float titleWidth = ImGui::CalcTextSize(POPUP_ID).x + (style.FramePadding.x * 2.0F);
    return std::max(widest + (style.WindowPadding.x * 2.0F), titleWidth);
}

bool render(State& state)
{
    const bool isOpen = ImGui::IsPopupOpen(POPUP_ID);
    if (!state.openRequested && !isOpen)
    {
        return false;
    }

    if (state.openRequested)
    {
        ImGui::OpenPopup(POPUP_ID);
        state.openRequested = false;
        state.centredDialogWidth = 0.0F; // Centre it afresh below
    }

    // As wide as its text needs, between ELEVATION_MIN_WIDTH_EM and 45 em -- exactly the former fixed
    // 480px at the reference configuration, which the long Linux text still fills (#1601) -- clamped
    // so a large font on a small window cannot push the dialog off-screen. This modal is the first thing a user
    // sees when running unelevated and it blocks input until dismissed, so its proportions matter
    // more than the usual cosmetic case (#937).
    //
    // Reapplied on every frame the popup is open, not once with ImGuiCond_Appearing. ImGui only lets
    // SetNextWindowSize override ImGuiWindowFlags_AlwaysAutoResize on frames where the size was
    // actually set by the API -- see size_auto_fit_x_always in imgui.cpp and the comment above it --
    // so a one-shot Appearing size is discarded by auto-fit from the second frame on and the clamp
    // never binds. Height stays 0 so it still auto-fits its content.
    if (ImGui::IsPopupOpen(POPUP_ID))
    {
        const ImGuiViewport* sizingViewport = ImGui::GetMainViewport();
        const float widthPx = UI::DialogMetrics::computeFittedDialogWidth(
            measureContentWidth(), ImGui::GetFontSize(), ELEVATION_MIN_WIDTH_EM, ELEVATION_WIDTH_EM, sizingViewport->WorkSize.x);
        ImGui::SetNextWindowSize(ImVec2(widthPx, 0.0F));

        // Centred on the TaskSmack window when it opens, and again whenever the window is resized or
        // the dialog's width changes (a font preset or display scale change) while it is open, so it
        // does not stay pinned to a corner of the old layout (#1601).
        // Sizes are compared to half a pixel: anything less doesn't move the centre visibly.
        constexpr float RECENTRE_THRESHOLD_PX = 0.5F;
        const auto moved = [](float now, float centredFor)
        {
            return std::abs(now - centredFor) > RECENTRE_THRESHOLD_PX;
        };
        // Its height too: the height auto-fits a frame or two after a width or font change, so a
        // centre taken on the frame the width changed is off by the height still to come.
        if (moved(widthPx, state.centredDialogWidth) || moved(sizingViewport->WorkSize.x, state.centredViewportWidth) ||
            moved(sizingViewport->WorkSize.y, state.centredViewportHeight) || moved(state.dialogHeight, state.centredDialogHeight))
        {
            ImGui::SetNextWindowPos(sizingViewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
            state.centredDialogWidth = widthPx;
            state.centredViewportWidth = sizingViewport->WorkSize.x;
            state.centredViewportHeight = sizingViewport->WorkSize.y;
            state.centredDialogHeight = state.dialogHeight;
        }
        // The height is held to the viewport too, every frame: the auto-fitted height grows with
        // the font and display scale, and without a cap the OK button could fall below the window
        // with this modal blocking everything else. Content that no longer fits scrolls (#1129).
        UI::Widgets::setNextDialogSizeConstraints(ImVec2(UI::DialogMetrics::computeDialogMaxExtent(sizingViewport->WorkSize.x),
                                                         UI::DialogMetrics::computeDialogMaxExtent(sizingViewport->WorkSize.y)));
    }

    bool dismissed = false;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;
    if (ImGui::BeginPopupModal(POPUP_ID, nullptr, flags))
    {
        UI::Widgets::keepCurrentWindowInViewport();
        state.dialogHeight = ImGui::GetWindowHeight();
        const auto& theme = UI::Theme::get();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);

        // Warning icon + header
        ImGui::TextColored(theme.scheme().textWarning, ICON_FA_LOCK "  Limited Data");
        ImGui::Separator();
        ImGui::Spacing();

        // Platform-specific body text
        constexpr std::string_view bodyText = ElevationNoticeText::forCurrentPlatform();

        // Wrapped to the window, because the width above is now a real constraint rather than a
        // suggestion: on a narrow window the clamp can leave less content width than the longest
        // line needs, and unwrapped text is simply clipped. The text has line breaks only between
        // paragraphs; within one, the wrap decides where lines end (#1200), where a hard-coded break
        // used to cut a line short beside a wrapped one.
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(bodyText.data(), bodyText.data() + bodyText.size());
        ImGui::PopTextWrapPos();

        UI::Widgets::sectionGap();

        // "Don't show again" checkbox
        ImGui::Checkbox("Don't show again", &state.dontShowAgain);
        ImGui::SetItemTooltip("Settings > Advanced > Show limited-data notice turns it back on");

        // The footer every dialog shares, OK on the right (#1200). The floor is 9.375 em, exactly the
        // former fixed 100px at the reference configuration; the measured-label term only takes over
        // if the label grows wider than that. A fixed-pixel button is a real interaction cost on a
        // HiDPI display (#937).
        const UI::Widgets::DialogFooterButton okButton{.label = "OK", .fills = nullptr, .tooltip = nullptr};
        if (UI::Widgets::dialogFooter(okButton, {}, UI::Widgets::footerButtonWidth({"OK"}, ELEVATION_BUTTON_MIN_EM)) ==
            UI::Widgets::DialogFooterAction::Primary)
        {
            dismissed = true;
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor(); // textPrimary
        ImGui::EndPopup();
    }
    return dismissed;
}

} // namespace App::ElevationNoticeDialog
