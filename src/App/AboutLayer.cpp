#include "App/AboutLayer.h"

#include "App/PlatformOpen.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "UI/IconLoader.h"
#include "UI/Theme.h"
#include "version.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <string>
#include <string_view>

namespace App
{

AboutLayer* AboutLayer::s_Instance = nullptr;

AboutLayer::AboutLayer() : Core::Layer("AboutLayer")
{}

AboutLayer::~AboutLayer() = default;

void AboutLayer::onAttach()
{
    // Layer lifecycle is guaranteed to be called from main thread only (SDL/ImGui requirement).
    // s_Instance is set by setInstance() immediately after pushLayer() returns.
    // During onAttach(), verify that either the singleton is not yet set (before setInstance),
    // or it already points to this instance (setInstance was called before pushLayer).
    assert((s_Instance == nullptr || s_Instance == this) && "AboutLayer singleton must be nullptr or point to this instance");
    loadIcon();
}

void AboutLayer::onDetach()
{
    // Clear singleton instance to avoid dangling pointer after this layer is destroyed.
    if (s_Instance == this)
    {
        s_Instance = nullptr;
    }
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

auto AboutLayer::instance() -> AboutLayer*
{
    return s_Instance;
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

    constexpr float marginPt = 24.0F;
    const ImGuiIO& io = ImGui::GetIO();
    const float pixelsPerPoint = 96.0F / 72.0F; // Approx. 96 DPI
    const float marginPx = marginPt * pixelsPerPoint * std::max(1.0F, io.FontGlobalScale);
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
        const float iconMax = 96.0F;
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

        // Center the OK button.
        constexpr float buttonWidth = 120.0F;
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
    const auto& exeDir = Core::Application::get().paths().executableDir();
    const auto cwd = std::filesystem::current_path();

    const std::array<std::filesystem::path, 3> baseDirs = {
        exeDir,               // installed layout (assets next to executable)
        exeDir.parent_path(), // build tree layout (bin/ + assets/ sibling)
        cwd,                  // running from repo root
    };

    constexpr std::array<std::string_view, 2> sizes = {"tasksmack-256.png", "tasksmack-128.png"};

    for (const auto& base : baseDirs)
    {
        for (const auto file : sizes)
        {
            const auto iconPath = base / "assets" / "icons" / file;

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
    }

    spdlog::warn("About dialog icon not found; continuing without image");
}

/// Set the singleton instance (non-owning; layer is owned by the application's layer stack).
/// THREAD-SAFETY: Must only be called from main thread during initialization,
/// before any code accesses instance().
void AboutLayer::setInstance(AboutLayer& layer)
{
    s_Instance = &layer;
}

} // namespace App
