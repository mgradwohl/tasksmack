#include "ElevationNoticeLayer.h"

#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <cassert>
#include <string_view>

namespace App
{

ElevationNoticeLayer* ElevationNoticeLayer::s_Instance = nullptr;

ElevationNoticeLayer::ElevationNoticeLayer() : Core::Layer("ElevationNoticeLayer")
{}

ElevationNoticeLayer::~ElevationNoticeLayer() = default;

void ElevationNoticeLayer::onAttach()
{
    // Layer lifecycle is guaranteed to be called from main thread only (SDL/ImGui requirement).
    // s_Instance is set by setInstance() immediately after pushLayer() returns.
    // During onAttach(), verify that either the singleton is not yet set (before setInstance),
    // or it already points to this instance (setInstance was called before pushLayer).
    assert((s_Instance == nullptr || s_Instance == this) && "ElevationNoticeLayer singleton must be nullptr or point to this instance");
}

void ElevationNoticeLayer::onDetach()
{
    // Clear singleton instance to avoid dangling pointer after this layer is destroyed.
    if (s_Instance == this)
    {
        s_Instance = nullptr;
    }
}

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

auto ElevationNoticeLayer::instance() -> ElevationNoticeLayer*
{
    return s_Instance;
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
        ImGui::SetNextWindowSize(ImVec2(480.0F, 0.0F), ImGuiCond_Appearing);

        ImGui::OpenPopup("Limited Data Available");
        m_OpenRequested = false;
    }

    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;
    if (ImGui::BeginPopupModal("Limited Data Available", nullptr, flags))
    {
        const auto& theme = UI::Theme::get();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);

        // Warning icon + header
        ImGui::TextColored(theme.scheme().textWarning, ICON_FA_LOCK "  Limited Data");
        ImGui::Separator();
        ImGui::Spacing();

        // Platform-specific body text
#ifdef __linux__
        constexpr std::string_view bodyText =
            "TaskSmack is running without elevated privileges.\n\n"
            "File descriptor counts and I/O statistics are\n"
            "unavailable for processes owned by other users.\n\n"
            "For complete data, run:\n"
            "    sudo tasksmack";
#elif defined(_WIN32)
        constexpr std::string_view bodyText =
            "TaskSmack is running without Administrator privileges.\n\n"
            "Per-process network statistics are unavailable.\n\n"
            "For complete data, run TaskSmack as Administrator.";
#else
        constexpr std::string_view bodyText =
            "TaskSmack is running without elevated privileges.\n\n"
            "Some per-process data may be unavailable.";
#endif

        ImGui::TextUnformatted(bodyText.data());

        ImGui::Spacing();
        ImGui::Spacing();

        // "Don't show again" checkbox
        ImGui::Checkbox("Don't show again", &m_DontShowAgain);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Right-align OK button
        const float buttonWidth = 100.0F;
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

/// Set the singleton instance (non-owning; layer is owned by the application's layer stack).
/// THREAD-SAFETY: Must only be called from main thread during initialization,
/// before any code accesses instance().
void ElevationNoticeLayer::setInstance(ElevationNoticeLayer& layer)
{
    s_Instance = &layer;
}

} // namespace App
