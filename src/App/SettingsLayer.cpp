#include "SettingsLayer.h"

#include "App/PlatformOpen.h"
#include "App/SettingsLayerDetail.h"
#include "App/UserConfig.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "UI/AssetPath.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <cassert>
#include <cstddef>
#include <filesystem>
#include <string>

namespace App
{

// Import Detail types and functions into this translation unit
using Detail::findFontSizeIndex;
using Detail::findHistoryIndex;
using Detail::findRefreshRateIndex;
using Detail::FONT_SIZE_OPTIONS;
using Detail::HISTORY_OPTIONS;
using Detail::REFRESH_RATE_OPTIONS;

namespace
{

// Get the themes directory path using multi-path asset resolution
[[nodiscard]] auto getThemesDir() -> std::filesystem::path
{
    return UI::findAssetsDir() / "themes";
}

} // namespace

SettingsLayer* SettingsLayer::s_Instance = nullptr;

SettingsLayer::SettingsLayer() : Core::Layer("SettingsLayer")
{}

SettingsLayer::~SettingsLayer() = default;

void SettingsLayer::onAttach()
{
    // Layer lifecycle is guaranteed to be called from main thread only (SDL/ImGui requirement).
    // s_Instance is set by setInstance() immediately after pushLayer() returns.
    // During onAttach(), verify that either the singleton is not yet set (before setInstance),
    // or it already points to this instance (setInstance was called before pushLayer).
    assert((s_Instance == nullptr || s_Instance == this) && "SettingsLayer singleton must be nullptr or point to this instance");
}

void SettingsLayer::onDetach()
{
    // Clear singleton instance to avoid dangling pointer after this layer is destroyed.
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

void SettingsLayer::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);

    // Listen for settings dialog requests
    dispatcher.dispatch<Core::OpenSettingsEvent>(
        [this](Core::OpenSettingsEvent&)
        {
            requestOpen();
            return false; // Don't consume
        });
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
    m_Themes = themeManager.discoveredThemes();
    m_SelectedThemeIndex = 0;

    for (std::size_t i = 0; i < m_Themes.size(); ++i)
    {
        if (m_Themes[i].id == settings.themeId)
        {
            m_SelectedThemeIndex = i;
            break;
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
    if (m_SelectedThemeIndex < m_Themes.size())
    {
        const std::string& newThemeId = m_Themes[m_SelectedThemeIndex].id;
        if (newThemeId != settings.themeId)
        {
            settings.themeId = newThemeId;
            themeManager.setThemeById(newThemeId);
            // Notify UI to invalidate theme-dependent caches
            {
                Core::ThemeChangedEvent event(newThemeId);
                Core::Application::get().raiseEvent(event);
            }
            spdlog::info("Theme changed to {}", newThemeId);
        }
    }

    // Apply font size
    const auto newFontSize = FONT_SIZE_OPTIONS[m_SelectedFontSizeIndex].value;
    if (newFontSize != settings.fontSize)
    {
        settings.fontSize = newFontSize;
        themeManager.setFontSize(newFontSize);
        // Notify panels to invalidate font-dependent caches
        {
            Core::FontSizeChangedEvent event(static_cast<int>(newFontSize));
            Core::Application::get().raiseEvent(event);
        }
        spdlog::info("Font size changed to {}", m_SelectedFontSizeIndex);
    }

    // Apply refresh rate
    const int newRefreshMs = REFRESH_RATE_OPTIONS[m_SelectedRefreshRateIndex].valueMs;
    if (newRefreshMs != settings.refreshIntervalMs)
    {
        settings.refreshIntervalMs = newRefreshMs;
        // Notify panels/samplers of interval change via event
        {
            Core::RefreshRateChangedEvent event(newRefreshMs);
            Core::Application::get().raiseEvent(event);
        }
        spdlog::info("Settings: Refresh rate changed to {} ms", newRefreshMs);
    }

    // Apply history duration
    const int newHistorySeconds = HISTORY_OPTIONS[m_SelectedHistoryIndex].valueSeconds;
    if (newHistorySeconds != settings.maxHistorySeconds)
    {
        settings.maxHistorySeconds = newHistorySeconds;
        // Notify panels/models to adjust history window via event
        {
            Core::HistoryDurationChangedEvent event(newHistorySeconds);
            Core::Application::get().raiseEvent(event);
        }
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

        if (!m_Themes.empty())
        {
            const char* currentTheme = m_Themes[m_SelectedThemeIndex].name.c_str();
            if (ImGui::BeginCombo("##Theme", currentTheme))
            {
                for (std::size_t i = 0; i < m_Themes.size(); ++i)
                {
                    const bool isSelected = (m_SelectedThemeIndex == i);
                    if (ImGui::Selectable(m_Themes[i].name.c_str(), isSelected))
                    {
                        m_SelectedThemeIndex = i;
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

        // NOLINT comments below: label is always initialized from a string literal, so .data() is null-terminated
        const char* currentFontSize =
            FONT_SIZE_OPTIONS[m_SelectedFontSizeIndex].label.data(); // NOLINT(bugprone-suspicious-stringview-data-usage)
        if (ImGui::BeginCombo("##FontSize", currentFontSize))
        {
            for (std::size_t i = 0; i < FONT_SIZE_OPTIONS.size(); ++i)
            {
                const bool isSelected = (m_SelectedFontSizeIndex == i);
                if (ImGui::Selectable(FONT_SIZE_OPTIONS[i].label.data(), isSelected)) // NOLINT(bugprone-suspicious-stringview-data-usage)
                {
                    m_SelectedFontSizeIndex = i;
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

        const char* currentRefresh =
            REFRESH_RATE_OPTIONS[m_SelectedRefreshRateIndex].label.data(); // NOLINT(bugprone-suspicious-stringview-data-usage)
        if (ImGui::BeginCombo("##RefreshRate", currentRefresh))
        {
            for (std::size_t i = 0; i < REFRESH_RATE_OPTIONS.size(); ++i)
            {
                const bool isSelected = (m_SelectedRefreshRateIndex == i);
                if (ImGui::Selectable(REFRESH_RATE_OPTIONS[i].label.data(), // NOLINT(bugprone-suspicious-stringview-data-usage)
                                      isSelected))
                {
                    m_SelectedRefreshRateIndex = i;
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

        const char* currentHistory =
            HISTORY_OPTIONS[m_SelectedHistoryIndex].label.data(); // NOLINT(bugprone-suspicious-stringview-data-usage)
        if (ImGui::BeginCombo("##History", currentHistory))
        {
            for (std::size_t i = 0; i < HISTORY_OPTIONS.size(); ++i)
            {
                const bool isSelected = (m_SelectedHistoryIndex == i);
                if (ImGui::Selectable(HISTORY_OPTIONS[i].label.data(), isSelected)) // NOLINT(bugprone-suspicious-stringview-data-usage)
                {
                    m_SelectedHistoryIndex = i;
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
        // Push text color to ensure visibility on button backgrounds
        ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);
        if (ImGui::Button(ICON_FA_FILE_PEN "  Edit Config File"))
        {
            // Result intentionally ignored - openWithSystemHandler logs warnings on failure
            (void) App::PlatformOpen::openWithSystemHandler(UserConfig::get().configPath());
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FOLDER "  Open Themes Folder"))
        {
            // Result intentionally ignored - openWithSystemHandler logs warnings on failure
            (void) App::PlatformOpen::openWithSystemHandler(getThemesDir());
        }
        ImGui::PopStyleColor();

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

        // Push text color to ensure visibility on button backgrounds
        ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0F)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        // Apply button with success color for positive action
        ImGui::PushStyleColor(ImGuiCol_Button, theme.scheme().successButton);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.scheme().successButtonHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.scheme().successButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);

        if (ImGui::Button("Apply", ImVec2(buttonWidth, 0.0F)))
        {
            applySettings();
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleColor(4); // Button, ButtonHovered, ButtonActive, Text

        ImGui::EndPopup();
    }
}

/// Set the singleton instance (non-owning; layer is owned by the application's layer stack).
/// THREAD-SAFETY: Must only be called from main thread during initialization,
/// before any code (onAttach's assert, onDetach's clear) reads s_Instance.
void SettingsLayer::setInstance(SettingsLayer& layer)
{
    s_Instance = &layer;
}

} // namespace App
