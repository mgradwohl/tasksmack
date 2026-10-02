#include "SettingsLayer.h"

#include "App/DialogGeometry.h"
#include "App/PlatformOpen.h"
#include "App/SettingsLayerDetail.h"
#include "App/UserConfig.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "Core/VideoBackend.h"
#include "UI/AssetPath.h"
#include "UI/DialogMetrics.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
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

// Button labels. The dialog's width is worked out from these before the buttons are drawn (see the
// combo sizing in renderSettingsDialog()), so each is named once and used for both.
constexpr const char* EDIT_CONFIG_LABEL = ICON_FA_FILE_PEN "  Edit Config File";
constexpr const char* OPEN_THEMES_LABEL = ICON_FA_FOLDER "  Open Themes Folder";
constexpr const char* CANCEL_LABEL = "Cancel";
constexpr const char* APPLY_LABEL = "Apply";
#ifndef _WIN32
constexpr const char* NATIVE_DECORATIONS_LABEL = "Use native window decorations instead of the custom title bar";
#endif

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
    m_ForceNativeDecorationsOnWayland = settings.forceNativeWindowDecorationsOnWayland;
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

    // Apply native-decorations preference (takes effect on next launch -- the window is
    // only created once, at startup)
    if (m_ForceNativeDecorationsOnWayland != settings.forceNativeWindowDecorationsOnWayland)
    {
        settings.forceNativeWindowDecorationsOnWayland = m_ForceNativeDecorationsOnWayland;
        spdlog::info("Settings: Force native window decorations on Wayland changed to {} (takes effect on next launch)",
                     m_ForceNativeDecorationsOnWayland);
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
    // No explicit width. The former fixed 450px is gone and nothing replaces it: the popup is
    // ImGuiWindowFlags_AlwaysAutoResize and every column below is measured from the text it has to
    // hold, so auto-fit already produces exactly the width the content needs at the current font.
    //
    // Deliberately not re-expressed as an em multiple. ImGui honours SetNextWindowSize over
    // AlwaysAutoResize only on frames where the size was genuinely set by the API, so an
    // ImGuiCond_Appearing width is discarded by auto-fit from the second frame on -- it would be
    // inert code that merely looked like it was doing something. See #947's review of the same
    // pattern in ElevationNoticeLayer, where the width is authored and so is reapplied every frame.

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

        // Column geometry, measured from the text it has to hold rather than fixed at 150/250px.
        // Those constants only looked right at one font size: at Small the labels used a fraction of
        // the 150px column and the 250px combos dwarfed values like "Small" and "250 ms", while at
        // Even Huger 150px was barely enough for "Metric Refresh Rate" (#921).
        //
        // Measuring is better than an em multiple here because these columns hold variable text: it
        // is self-documenting, it tracks the theme list and option arrays if either gains an entry,
        // and it absorbs the glyph-metric differences between platforms automatically.
        const float emPx = ImGui::GetFontSize();
        const float labelGap = style.ItemSpacing.x * 2.0F;
        const float widestLabel = std::max({ImGui::CalcTextSize("Theme").x,
                                            ImGui::CalcTextSize("Font Size").x,
                                            ImGui::CalcTextSize("Metric Refresh Rate").x,
                                            ImGui::CalcTextSize("Metric History").x});
        const float valueColumn = UI::DialogMetrics::computeValueColumnStart(widestLabel, labelGap);

        // What a combo needs beyond its text: ImGui's frame padding either side, plus the arrow
        // button, which it draws as a square of the frame height.
        const float comboDecoration = (style.FramePadding.x * 2.0F) + ImGui::GetFrameHeight();

        float widestAppearanceValue = 0.0F;
        for (const auto& themeEntry : m_Themes)
        {
            widestAppearanceValue = std::max(widestAppearanceValue, ImGui::CalcTextSize(themeEntry.name.c_str()).x);
        }
        for (const auto& option : FONT_SIZE_OPTIONS)
        {
            widestAppearanceValue =
                std::max(widestAppearanceValue, ImGui::CalcTextSize(option.label.data(), option.label.data() + option.label.size()).x);
        }
        // Capped against the viewport. Theme names are read from a user's TOML with no length limit
        // (ThemeLoader), so measuring them is unbounded: a long name would otherwise widen this
        // auto-resizing popup past the window and put the combo's arrow and the buttons below it out
        // of reach. The floor keeps the control usable if the cap bites; ImGui clips the combo's
        // preview text, so a long name degrades to truncation rather than an unreachable control.
        const float comboMinWidth = (MIN_COMBO_EM * emPx) + comboDecoration;

        // The dialog auto-fits its widest row, and that is usually not a combo row: the two
        // ADVANCED buttons and the Cancel/Apply pair are both wider. Sized only to their own text,
        // the combos stopped short of the dialog's right edge, in line with neither the separators
        // nor Apply (#972). Those rows are measured from text as well, so the width they will give
        // the dialog is known here, and the combos are widened to reach it.
        const float advancedRowWidth = ImGui::CalcTextSize(EDIT_CONFIG_LABEL).x + ImGui::CalcTextSize(OPEN_THEMES_LABEL).x +
                                       (style.FramePadding.x * 4.0F) + style.ItemSpacing.x;
        const float actionButtonWidth =
            std::max(UI::DialogMetrics::computeActionButtonWidth(ImGui::CalcTextSize(CANCEL_LABEL).x, emPx, SETTINGS_BUTTON_MIN_EM),
                     UI::DialogMetrics::computeActionButtonWidth(ImGui::CalcTextSize(APPLY_LABEL).x, emPx, SETTINGS_BUTTON_MIN_EM));
        const float actionRowWidth = (actionButtonWidth * 2.0F) + style.ItemSpacing.x;
        // On native Wayland the ADVANCED section also has a checkbox, and its label makes that row
        // the widest in the dialog. A checkbox is a square of the frame height, then its label.
#ifndef _WIN32
        const float checkboxRowWidth =
            Core::VideoBackend::isWayland()
                ? (ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(NATIVE_DECORATIONS_LABEL).x)
                : 0.0F;
#else
        const float checkboxRowWidth = 0.0F;
#endif
        const float widestOtherRow = std::max({advancedRowWidth, actionRowWidth, checkboxRowWidth});

        const float appearanceComboWidth = UI::DialogMetrics::computeCappedControlWidth(
            UI::DialogMetrics::computeFilledControlWidth(
                widestAppearanceValue + comboDecoration, valueColumn, style.WindowPadding.x, widestOtherRow),
            valueColumn,
            style.WindowPadding.x * 2.0F,
            viewport->WorkSize.x,
            comboMinWidth);

        // Theme dropdown
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Theme");
        ImGui::SameLine(valueColumn);
        ImGui::SetNextItemWidth(appearanceComboWidth);

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
        ImGui::SameLine(valueColumn);
        ImGui::SetNextItemWidth(appearanceComboWidth);

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
        // The performance combos hold much shorter values ("250 ms", "5 minutes") than the theme
        // names above, so they get their own measured width and keep the established look by sharing
        // the Appearance combos' right edge.
        float widestPerfValue = 0.0F;
        for (const auto& option : REFRESH_RATE_OPTIONS)
        {
            widestPerfValue =
                std::max(widestPerfValue, ImGui::CalcTextSize(option.label.data(), option.label.data() + option.label.size()).x);
        }
        for (const auto& option : HISTORY_OPTIONS)
        {
            widestPerfValue =
                std::max(widestPerfValue, ImGui::CalcTextSize(option.label.data(), option.label.data() + option.label.size()).x);
        }
        const float perfComboWidth = UI::DialogMetrics::computeCappedControlWidth(
            widestPerfValue + comboDecoration, valueColumn, style.WindowPadding.x * 2.0F, viewport->WorkSize.x, comboMinWidth);
        const float perfLabelWidth = UI::DialogMetrics::computeRightAlignedStart(valueColumn, appearanceComboWidth, perfComboWidth);

        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_GAUGE_HIGH "  PERFORMANCE");
        ImGui::Separator();
        ImGui::Spacing();

        // Metric Refresh Rate dropdown
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Metric Refresh Rate");
        ImGui::SameLine(perfLabelWidth);
        ImGui::SetNextItemWidth(perfComboWidth);

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
        ImGui::SetNextItemWidth(perfComboWidth);

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
        if (ImGui::Button(EDIT_CONFIG_LABEL))
        {
            // Result intentionally ignored - openWithSystemHandler logs warnings on failure
            (void) App::PlatformOpen::openWithSystemHandler(UserConfig::get().configPath());
        }
        ImGui::SameLine();
        if (ImGui::Button(OPEN_THEMES_LABEL))
        {
            // Result intentionally ignored - openWithSystemHandler logs warnings on failure
            (void) App::PlatformOpen::openWithSystemHandler(getThemesDir());
        }
        ImGui::PopStyleColor();

#ifndef _WIN32
        // Only meaningful on native Wayland -- the custom title bar's drag/resize
        // implementation depends on compositor hand-off there in ways that don't apply
        // to X11/XWayland (see #744, #749). Hidden elsewhere since it would have no effect.
        if (Core::VideoBackend::isWayland())
        {
            ImGui::Spacing();
            ImGui::Checkbox(NATIVE_DECORATIONS_LABEL, &m_ForceNativeDecorationsOnWayland);
            ImGui::TextColored(theme.scheme().textMuted, "Takes effect after restarting TaskSmack.");
        }
#endif

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ========================================
        // Buttons
        // ========================================
        // Floor of 9.375 em is exactly the former fixed 100px at the reference configuration; the
        // measured term takes over for whichever of the two labels is wider once the font grows.
        // (Computed above as actionButtonWidth, where the combos need it to find the dialog's width.)
        const float buttonWidth = actionButtonWidth;
        const float totalButtonWidth = actionRowWidth;
        const float availWidth = ImGui::GetContentRegionAvail().x;

        // Right-align buttons
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availWidth - totalButtonWidth);

        // Push text color to ensure visibility on button backgrounds
        ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);
        if (ImGui::Button(CANCEL_LABEL, ImVec2(buttonWidth, 0.0F)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        // Apply button with success color for positive action. Its label is drawn in whichever of
        // the theme's two poles -- its text colour or its window background -- reads better on the
        // fill showing in the button's current state; the ordinary text colour was nearly invisible
        // on it in most of the bundled themes (#969).
        if (UI::Widgets::filledButton(APPLY_LABEL,
                                      ImVec2(buttonWidth, 0.0F),
                                      {
                                          .resting = theme.scheme().successButton,
                                          .hovered = theme.scheme().successButtonHovered,
                                          .pressed = theme.scheme().successButtonActive,
                                      },
                                      theme.scheme().textPrimary,
                                      theme.scheme().windowBg))
        {
            applySettings();
            ImGui::CloseCurrentPopup();
        }

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
