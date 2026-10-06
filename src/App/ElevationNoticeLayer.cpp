#include "ElevationNoticeLayer.h"

#include "App/DialogGeometry.h"
#include "App/ElevationNoticeText.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "UI/DialogMetrics.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <string_view>

namespace App
{

ElevationNoticeLayer::ElevationNoticeLayer() : Core::Layer("ElevationNoticeLayer")
{}

ElevationNoticeLayer::~ElevationNoticeLayer() = default;

void ElevationNoticeLayer::onUpdate([[maybe_unused]] float deltaTime)
{
    // No-op
}

void ElevationNoticeLayer::onRender()
{
    renderDialog();
}

void ElevationNoticeLayer::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);

    dispatcher.dispatch<Core::OpenElevationNoticeEvent>(
        [this](Core::OpenElevationNoticeEvent&)
        {
            requestOpen();
            return false; // Don't consume; allow other layers to observe
        });
}

void ElevationNoticeLayer::requestOpen()
{
    m_OpenRequested = true;
    m_DontShowAgain = false;
}

void ElevationNoticeLayer::renderDialog()
{
    const bool isOpen = ImGui::IsPopupOpen("Limited Data Available");
    if (!m_OpenRequested && !isOpen)
    {
        return;
    }

    if (m_OpenRequested)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
        ImGui::OpenPopup("Limited Data Available");
        m_OpenRequested = false;
    }

    // 45 em is exactly the former fixed 480px at the reference configuration, clamped so a large
    // font on a small window cannot push the dialog off-screen. This modal is the first thing a user
    // sees when running unelevated and it blocks input until dismissed, so its proportions matter
    // more than the usual cosmetic case (#937).
    //
    // Reapplied on every frame the popup is open, not once with ImGuiCond_Appearing. ImGui only lets
    // SetNextWindowSize override ImGuiWindowFlags_AlwaysAutoResize on frames where the size was
    // actually set by the API -- see size_auto_fit_x_always in imgui.cpp and the comment above it --
    // so a one-shot Appearing size is discarded by auto-fit from the second frame on and the clamp
    // never binds. Height stays 0 so it still auto-fits its content.
    if (ImGui::IsPopupOpen("Limited Data Available"))
    {
        const ImGuiViewport* sizingViewport = ImGui::GetMainViewport();
        const float widthPx = UI::DialogMetrics::computeDialogWidth(ImGui::GetFontSize(), ELEVATION_WIDTH_EM, sizingViewport->WorkSize.x);
        ImGui::SetNextWindowSize(ImVec2(widthPx, 0.0F));
        // The height is held to the viewport too, every frame: the auto-fitted height grows with
        // the font and display scale, and without a cap the OK button could fall below the window
        // with this modal blocking everything else. Content that no longer fits scrolls (#1129).
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0F, 0.0F),
                                            ImVec2(UI::DialogMetrics::computeDialogMaxExtent(sizingViewport->WorkSize.x),
                                                   UI::DialogMetrics::computeDialogMaxExtent(sizingViewport->WorkSize.y)));
    }

    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;
    if (ImGui::BeginPopupModal("Limited Data Available", nullptr, flags))
    {
        UI::Widgets::keepCurrentWindowInViewport();
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
        // line needs, and unwrapped text is simply clipped. The explicit blank lines in bodyText
        // still separate the paragraphs; wrapping only reflows within them.
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextUnformatted(bodyText.data(), bodyText.data() + bodyText.size());
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::Spacing();

        // "Don't show again" checkbox
        ImGui::Checkbox("Don't show again", &m_DontShowAgain);
        ImGui::SetItemTooltip("Settings > Advanced > Show limited-data notice turns it back on");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Right-align OK button. The floor is 9.375 em, exactly the former fixed 100px at the
        // reference configuration; the measured-label term only takes over if the label grows wider
        // than that. A fixed-pixel button is a real interaction cost on a HiDPI display (#937).
        const float buttonWidth =
            UI::DialogMetrics::computeActionButtonWidth(ImGui::CalcTextSize("OK").x, ImGui::GetFontSize(), ELEVATION_BUTTON_MIN_EM);
        const float availX = ImGui::GetContentRegionAvail().x;
        const float offset = std::max(0.0F, availX - buttonWidth);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

        ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);
        if (ImGui::Button("OK", ImVec2(buttonWidth, 0.0F)))
        {
            if (m_DontShowAgain)
            {
                auto& config = UserConfig::get();
                config.settings().showPrivilegeNotice = false;
                config.save();
                spdlog::info("ElevationNoticeLayer: user suppressed privilege notice");
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();

        ImGui::PopStyleColor(); // textPrimary
        ImGui::EndPopup();
    }
}

} // namespace App
