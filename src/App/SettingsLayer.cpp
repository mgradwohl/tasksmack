#include "SettingsLayer.h"

#include "App/UserConfig.h"
#include "Platform/Factory.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>

#ifdef __linux__
#include <unistd.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <shellapi.h>
#include <windows.h>
#endif

namespace App
{

namespace
{

// Font size options
struct FontSizeOption
{
    const char* label;
    UI::FontSize value;
};

constexpr std::array<FontSizeOption, 6> FONT_SIZE_OPTIONS = {{
    {.label = "Small", .value = UI::FontSize::Small},
    {.label = "Medium", .value = UI::FontSize::Medium},
    {.label = "Large", .value = UI::FontSize::Large},
    {.label = "Extra Large", .value = UI::FontSize::ExtraLarge},
    {.label = "Huge", .value = UI::FontSize::Huge},
    {.label = "Even Huger", .value = UI::FontSize::EvenHuger},
}};

// Refresh rate options (milliseconds)
struct RefreshRateOption
{
    const char* label;
    int valueMs;
};

constexpr std::array<RefreshRateOption, 6> REFRESH_RATE_OPTIONS = {{
    {.label = "100 ms", .valueMs = 100},
    {.label = "250 ms", .valueMs = 250},
    {.label = "500 ms", .valueMs = 500},
    {.label = "1 second", .valueMs = 1000},
    {.label = "2 seconds", .valueMs = 2000},
    {.label = "5 seconds", .valueMs = 5000},
}};

// History duration options (seconds)
struct HistoryOption
{
    const char* label;
    int valueSeconds;
};

constexpr std::array<HistoryOption, 4> HISTORY_OPTIONS = {{
    {.label = "1 minute", .valueSeconds = 60},
    {.label = "2 minutes", .valueSeconds = 120},
    {.label = "5 minutes", .valueSeconds = 300},
    {.label = "10 minutes", .valueSeconds = 600},
}};

// Helper to find index by value
[[nodiscard]] auto findFontSizeIndex(UI::FontSize size) -> int
{
    for (size_t i = 0; i < FONT_SIZE_OPTIONS.size(); ++i)
    {
        if (FONT_SIZE_OPTIONS[i].value == size)
        {
            return static_cast<int>(i);
        }
    }
    return 1; // Default to Medium
}

[[nodiscard]] auto findRefreshRateIndex(int ms) -> int
{
    for (size_t i = 0; i < REFRESH_RATE_OPTIONS.size(); ++i)
    {
        if (REFRESH_RATE_OPTIONS[i].valueMs == ms)
        {
            return static_cast<int>(i);
        }
    }
    return 3; // Default to 1 second
}

[[nodiscard]] auto findHistoryIndex(int seconds) -> int
{
    for (size_t i = 0; i < HISTORY_OPTIONS.size(); ++i)
    {
        if (HISTORY_OPTIONS[i].valueSeconds == seconds)
        {
            return static_cast<int>(i);
        }
    }
    return 2; // Default to 5 minutes
}

// Get the themes directory path (relative to executable)
[[nodiscard]] auto getThemesDir() -> std::filesystem::path
{
    auto provider = Platform::makePathProvider();
    return provider->getExecutableDir() / "assets" / "themes";
}

// Open a file or folder with the system default handler
void openPath(const std::filesystem::path& path)
{
    const std::string pathStr = path.string();

#ifdef _WIN32
    const std::wstring widePath(pathStr.begin(), pathStr.end());
    ShellExecuteW(nullptr, L"open", widePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    const pid_t pid = ::fork();
    if (pid == 0)
    {
        // Child: exec xdg-open; if it fails, exit quietly.
        ::execlp("xdg-open", "xdg-open", pathStr.c_str(), nullptr);
        _exit(127);
    }
    else if (pid < 0)
    {
        spdlog::warn("Failed to launch xdg-open for path: {}", pathStr);
    }
#endif
}

} // namespace

SettingsLayer* SettingsLayer::s_Instance = nullptr;

SettingsLayer::SettingsLayer() : Core::Layer("SettingsLayer")
{
}

SettingsLayer::~SettingsLayer()
{
    if (s_Instance == this)
    {
        s_Instance = nullptr;
    }
}

void SettingsLayer::onAttach()
{
    assert(s_Instance == nullptr && "SettingsLayer instance already exists!");
    s_Instance = this;
}

void SettingsLayer::onDetach()
{
    if (s_Instance == this)
    {
        s_Instance = nullptr;
    }
}

void SettingsLayer::onUpdate([[maybe_unused]] float deltaTime)
{
    // No-op
}

void SettingsLayer::onRender()
{
    renderSettingsDialog();
}

auto SettingsLayer::instance() -> SettingsLayer*
{
    return s_Instance;
}

void SettingsLayer::requestOpen()
{
    m_OpenRequested = true;
    loadCurrentSettings();
}

void SettingsLayer::loadCurrentSettings()
{
    const auto& config = UserConfig::get();
    const auto& settings = config.settings();
    auto& themeManager = UI::Theme::get();

    // Load theme options
    m_ThemeNames.clear();
    m_ThemeIds.clear();
    m_SelectedThemeIndex = 0;

    const auto& themes = themeManager.discoveredThemes();
    for (size_t i = 0; i < themes.size(); ++i)
    {
        m_ThemeNames.push_back(themes[i].name);
        m_ThemeIds.push_back(themes[i].id);
        if (themes[i].id == settings.themeId)
        {
            m_SelectedThemeIndex = static_cast<int>(i);
        }
    }

    // Load other settings
    m_SelectedFontSizeIndex = findFontSizeIndex(settings.fontSize);
    m_SelectedRefreshRateIndex = findRefreshRateIndex(settings.refreshIntervalMs);
    m_SelectedHistoryIndex = findHistoryIndex(settings.maxHistorySeconds);
}

void SettingsLayer::applySettings()
{
    auto& config = UserConfig::get();
    auto& settings = config.settings();
    auto& themeManager = UI::Theme::get();

    // Apply theme
    if (m_SelectedThemeIndex >= 0 && static_cast<size_t>(m_SelectedThemeIndex) < m_ThemeIds.size())
    {
        const std::string& newThemeId = m_ThemeIds[static_cast<size_t>(m_SelectedThemeIndex)];
        if (newThemeId != settings.themeId)
        {
            settings.themeId = newThemeId;
            themeManager.setThemeById(newThemeId);
            spdlog::info("Settings: Theme changed to {}", newThemeId);
        }
    }

    // Apply font size
    const auto newFontSize = FONT_SIZE_OPTIONS[static_cast<size_t>(m_SelectedFontSizeIndex)].value;
    if (newFontSize != settings.fontSize)
    {
        settings.fontSize = newFontSize;
        themeManager.setFontSize(newFontSize);
        spdlog::info("Settings: Font size changed to {}", m_SelectedFontSizeIndex);
    }

    // Apply refresh rate
    const int newRefreshMs = REFRESH_RATE_OPTIONS[static_cast<size_t>(m_SelectedRefreshRateIndex)].valueMs;
    if (newRefreshMs != settings.refreshIntervalMs)
    {
        settings.refreshIntervalMs = newRefreshMs;
        spdlog::info("Settings: Refresh rate changed to {} ms", newRefreshMs);
    }

    // Apply history duration
    const int newHistorySeconds = HISTORY_OPTIONS[static_cast<size_t>(m_SelectedHistoryIndex)].valueSeconds;
    if (newHistorySeconds != settings.maxHistorySeconds)
    {
        settings.maxHistorySeconds = newHistorySeconds;
        spdlog::info("Settings: History duration changed to {} seconds", newHistorySeconds);
    }

    // Save to disk
    config.save();
}

void SettingsLayer::renderSettingsDialog()
{
    const bool isOpen = ImGui::IsPopupOpen("Settings");
    if (!m_OpenRequested && !isOpen)
    {
        return;
    }

    if (m_OpenRequested)
    {
        ImGui::OpenPopup("Settings");
        m_OpenRequested = false;
    }

    // Center the popup
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(450.0F, 0.0F), ImGuiCond_Appearing);

    const ImGuiWindowFlags popupFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove;

    if (ImGui::BeginPopupModal("Settings", nullptr, popupFlags))
    {
        const auto& theme = UI::Theme::get();
        const ImGuiStyle& style = ImGui::GetStyle();

        // ========================================
        // APPEARANCE Section
        // ========================================
        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_PALETTE "  APPEARANCE");
        ImGui::Separator();
        ImGui::Spacing();

        // Theme dropdown
        constexpr float LABEL_WIDTH = 150.0F;
        constexpr float COMBO_WIDTH = 250.0F;

        ImGui::AlignTextToFramePadding();
        ImGui::Text("Theme");
        ImGui::SameLine(LABEL_WIDTH);
        ImGui::SetNextItemWidth(COMBO_WIDTH);

        if (!m_ThemeNames.empty())
        {
            const char* currentTheme = m_ThemeNames[static_cast<size_t>(m_SelectedThemeIndex)].c_str();
            if (ImGui::BeginCombo("##Theme", currentTheme))
            {
                for (size_t i = 0; i < m_ThemeNames.size(); ++i)
                {
                    const bool isSelected = std::cmp_equal(m_SelectedThemeIndex, i);
                    if (ImGui::Selectable(m_ThemeNames[i].c_str(), isSelected))
                    {
                        m_SelectedThemeIndex = static_cast<int>(i);
                    }
                    if (isSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }

        ImGui::Spacing();

        // Font Size dropdown
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Font Size");
        ImGui::SameLine(LABEL_WIDTH);
        ImGui::SetNextItemWidth(COMBO_WIDTH);

        const char* currentFontSize = FONT_SIZE_OPTIONS[static_cast<size_t>(m_SelectedFontSizeIndex)].label;
        if (ImGui::BeginCombo("##FontSize", currentFontSize))
        {
            for (size_t i = 0; i < FONT_SIZE_OPTIONS.size(); ++i)
            {
                const bool isSelected = std::cmp_equal(m_SelectedFontSizeIndex, i);
                if (ImGui::Selectable(FONT_SIZE_OPTIONS[i].label, isSelected))
                {
                    m_SelectedFontSizeIndex = static_cast<int>(i);
                }
                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();

        // ========================================
        // PERFORMANCE Section
        // ========================================
        constexpr float PERF_COMBO_WIDTH = 150.0F;
        // Right-align with Appearance combos: start at LABEL_WIDTH + (COMBO_WIDTH - PERF_COMBO_WIDTH)
        const float perfLabelWidth = LABEL_WIDTH + (COMBO_WIDTH - PERF_COMBO_WIDTH);

        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_GAUGE_HIGH "  PERFORMANCE");
        ImGui::Separator();
        ImGui::Spacing();

        // Metric Refresh Rate dropdown
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Metric Refresh Rate");
        ImGui::SameLine(perfLabelWidth);
        ImGui::SetNextItemWidth(PERF_COMBO_WIDTH);

        const char* currentRefresh = REFRESH_RATE_OPTIONS[static_cast<size_t>(m_SelectedRefreshRateIndex)].label;
        if (ImGui::BeginCombo("##RefreshRate", currentRefresh))
        {
            for (size_t i = 0; i < REFRESH_RATE_OPTIONS.size(); ++i)
            {
                const bool isSelected = std::cmp_equal(m_SelectedRefreshRateIndex, i);
                if (ImGui::Selectable(REFRESH_RATE_OPTIONS[i].label, isSelected))
                {
                    m_SelectedRefreshRateIndex = static_cast<int>(i);
                }
                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();

        // Metric History Duration dropdown
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Metric History");
        ImGui::SameLine(perfLabelWidth);
        ImGui::SetNextItemWidth(PERF_COMBO_WIDTH);

        const char* currentHistory = HISTORY_OPTIONS[static_cast<size_t>(m_SelectedHistoryIndex)].label;
        if (ImGui::BeginCombo("##History", currentHistory))
        {
            for (size_t i = 0; i < HISTORY_OPTIONS.size(); ++i)
            {
                const bool isSelected = std::cmp_equal(m_SelectedHistoryIndex, i);
                if (ImGui::Selectable(HISTORY_OPTIONS[i].label, isSelected))
                {
                    m_SelectedHistoryIndex = static_cast<int>(i);
                }
                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();

        // ========================================
        // ADVANCED Section
        // ========================================
        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_FOLDER_OPEN "  ADVANCED");
        ImGui::Separator();
        ImGui::Spacing();

        // Button row for config file and themes folder
        if (ImGui::Button(ICON_FA_FILE_PEN "  Edit Config File"))
        {
            openPath(UserConfig::get().configPath());
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FOLDER "  Open Themes Folder"))
        {
            openPath(getThemesDir());
        }

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ========================================
        // Buttons
        // ========================================
        const float buttonWidth = 100.0F;
        const float totalButtonWidth = (buttonWidth * 2.0F) + style.ItemSpacing.x;
        const float availWidth = ImGui::GetContentRegionAvail().x;

        // Right-align buttons
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availWidth - totalButtonWidth);

        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0F)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        // Apply button with success color for positive action
        ImGui::PushStyleColor(ImGuiCol_Button, theme.scheme().successButton);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.scheme().successButtonHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.scheme().successButtonActive);

        if (ImGui::Button("Apply", ImVec2(buttonWidth, 0.0F)))
        {
            applySettings();
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor(3);

        ImGui::EndPopup();
    }
}

} // namespace App
