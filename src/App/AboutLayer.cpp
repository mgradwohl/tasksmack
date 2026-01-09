#include "App/AboutLayer.h"

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
#include <system_error>

// NOLINTBEGIN(misc-include-cleaner) - POSIX headers: include-cleaner lacks mappings for pid_t
#ifdef __linux__
#include <sys/types.h>
#include <unistd.h>
#endif
// NOLINTEND(misc-include-cleaner)

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <shellapi.h>
#include <windows.h>
#endif

namespace
{

[[nodiscard]] std::filesystem::path getExecutableDir()
{
#ifdef __linux__
    std::error_code errorCode;
    auto exePath = std::filesystem::read_symlink("/proc/self/exe", errorCode);
    if (!errorCode)
    {
        return exePath.parent_path();
    }
#elif defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    // Note: GetModuleFileNameW returns DWORD; explicit cast is safe for path lengths.
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len > 0 && len < buffer.size())
    {
        buffer.resize(len);
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

} // namespace

namespace App
{

AboutLayer* AboutLayer::s_Instance = nullptr;

AboutLayer::AboutLayer() : Core::Layer("AboutLayer")
{
}

AboutLayer::~AboutLayer()
{
    if (s_Instance == this)
    {
        s_Instance = nullptr;
    }
}

void AboutLayer::onAttach()
{
    // Layer lifecycle is guaranteed to be called from main thread only (GLFW requirement).
    // Enforce single instance with assertion rather than atomic operations.
    assert(s_Instance == nullptr && "AboutLayer instance already exists!");
    s_Instance = this;
    loadIcon();
}

void AboutLayer::onDetach()
{
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
            openUrl(repoUrl);
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
    const auto exeDir = getExecutableDir();
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

void AboutLayer::openUrl(const std::string& url)
{
#ifdef _WIN32
    const std::wstring wideUrl(url.begin(), url.end());
    ShellExecuteW(nullptr, L"open", wideUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    // NOLINTNEXTLINE(misc-include-cleaner) - pid_t from sys/types.h, include-cleaner false positive
    const pid_t pid = ::fork();
    if (pid == 0)
    {
        // Child: exec xdg-open; if it fails, exit quietly.
        ::execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
        _exit(127);
    }
    else if (pid < 0)
    {
        spdlog::warn("Failed to launch xdg-open for URL: {}", url);
    }
#endif
}

} // namespace App
