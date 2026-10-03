#include "App/AboutLayer.h"

#include "App/DialogGeometry.h"
#include "App/PlatformOpen.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "UI/AssetPath.h"
#include "UI/DialogMetrics.h"
#include "UI/IconLoader.h"
#include "UI/Theme.h"
#include "version.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace App
{

AboutLayer::AboutLayer() : Core::Layer("AboutLayer")
{}

AboutLayer::~AboutLayer() = default;

void AboutLayer::onAttach()
{
    loadIcon();
}

void AboutLayer::onUpdate([[maybe_unused]] float deltaTime)
{
    // No-op
}

void AboutLayer::onRender()
{
    renderAboutDialog();
}

void AboutLayer::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);

    // Listen for about/help requests
    dispatcher.dispatch<Core::OpenAboutEvent>(
        [this](Core::OpenAboutEvent&)
        {
            requestOpen();
            return false; // Don't consume
        });
}

void AboutLayer::requestOpen()
{
    m_OpenRequested = true;
}

void AboutLayer::renderAboutDialog()
{
    const bool isOpen = ImGui::IsPopupOpen("About TaskSmack");
    if (!m_OpenRequested && !isOpen)
    {
        return; // Do nothing when not visible and not requested
    }

    if (m_OpenRequested)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 center = viewport->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));

        ImGui::OpenPopup("About TaskSmack");
        m_OpenRequested = false;
    }

    // Margin around the dialog's contents. Formerly 24pt converted through a literal 96/72 with the
    // comment "Approx. 96 DPI", which is simply wrong on any scaled display, and then multiplied by
    // io.FontGlobalScale clamped to >= 1.0 -- a stand-in for the font size that cannot compensate
    // downward and is not the font size anyway. One em already carries both the Font Size setting
    // and the display density. 3 em is exactly the 32px that expression produced at the reference
    // configuration, so the dialog is unchanged there (#935).
    const float emPx = ImGui::GetFontSize();
    const float marginPx = ABOUT_MARGIN_EM * emPx;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(marginPx, marginPx));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;
    if (ImGui::BeginPopupModal("About TaskSmack", nullptr, flags))
    {

        const auto& theme = UI::Theme::get();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);

        ImFont* titleFont = UI::Theme::get().largeFont();
        const float lineGap = ImGui::GetStyle().ItemSpacing.y;
        const float titleHeight = ImGui::GetTextLineHeight();
        const float iconVerticalOffset = titleHeight + (lineGap * 2.0F);

        // Icon on the left
        ImGui::BeginGroup();
        // 9 em is exactly the former fixed 96px at the reference configuration, so the icon now
        // grows and shrinks with the text beside it instead of towering over it at Small and being
        // swamped by it at Even Huger (#935).
        const float iconMax = ABOUT_ICON_EM * emPx;
        ImGui::Dummy(ImVec2(0.0F, iconVerticalOffset));
        if (m_Icon.valid())
        {
            const ImVec2 rawSize = m_Icon.size();
            const float scale = std::min(iconMax / rawSize.x, iconMax / rawSize.y);
            const ImVec2 drawSize(rawSize.x * scale, rawSize.y * scale);
            ImGui::Image(m_Icon.textureId(), drawSize);
        }
        else
        {
            ImGui::Dummy(ImVec2(iconMax, iconMax));
        }
        ImGui::EndGroup();

        ImGui::SameLine();

        // Text on the right
        ImGui::BeginGroup();
        if (titleFont != nullptr)
        {
            ImGui::PushFont(titleFont);
            ImGui::TextUnformatted("TaskSmack");
            ImGui::PopFont();
        }
        else
        {
            ImGui::TextUnformatted("TaskSmack");
        }

        ImGui::Dummy(ImVec2(0.0F, lineGap));

        ImGui::Text("%s (%s build)", tasksmack::Version::STRING, tasksmack::Version::BUILD_TYPE);
        ImGui::TextUnformatted("TaskSmack: the cross-platform system monitor");

        ImGui::Spacing();

        constexpr const char* repoUrl = "https://github.com/mgradwohl/tasksmack";
        ImGui::PushStyleColor(ImGuiCol_Text, theme.accentColor(0));
        if (ImGui::Selectable(repoUrl, false, ImGuiSelectableFlags_DontClosePopups))
        {
            (void) App::PlatformOpen::openWithSystemHandler(std::string_view{repoUrl});
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        ImGui::PopStyleColor();

        ImGui::Text("License: MIT");
        ImGui::Text("Commit: %s", tasksmack::Version::GIT_COMMIT);
        ImGui::TextUnformatted("Font: Inter (SIL Open Font License 1.1)");

        ImGui::EndGroup();

        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0.0F, marginPx));

        // Center the OK button. The floor is 11.25 em, exactly the former fixed 120px at the
        // reference configuration; the measured-label term only takes over if a translation or a
        // very large font makes the label wider than that (#935).
        const float buttonWidth = UI::DialogMetrics::computeActionButtonWidth(ImGui::CalcTextSize("OK").x, emPx, ABOUT_BUTTON_MIN_EM);
        const float availX = ImGui::GetContentRegionAvail().x;
        const float offset = std::max(0.0F, (availX - buttonWidth) * 0.5F);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        if (ImGui::Button("OK", ImVec2(buttonWidth, 0.0F)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
    else
    {
        ImGui::PopStyleVar();
    }
}

void AboutLayer::loadIcon()
{
    const auto iconsDir = UI::findAssetsDir() / "icons";

    constexpr std::array<const char*, 2> sizes = {"tasksmack-256.png", "tasksmack-128.png"};

    for (const auto* const file : sizes)
    {
        const auto iconPath = iconsDir / file;

        if (!std::filesystem::exists(iconPath))
        {
            continue;
        }

        m_Icon = UI::loadTexture(iconPath);
        if (m_Icon.valid())
        {
            spdlog::info("Loaded About dialog icon: {} ({}x{})",
                         iconPath.string(),
                         static_cast<int>(m_Icon.size().x),
                         static_cast<int>(m_Icon.size().y));
            return;
        }
    }

    spdlog::warn("About dialog icon not found; continuing without image");
}

} // namespace App
