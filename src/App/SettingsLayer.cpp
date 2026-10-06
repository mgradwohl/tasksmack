#include "SettingsLayer.h"

#include "App/DialogGeometry.h"
#include "App/FontSizeChange.h"
#include "App/PlatformOpen.h"
#include "App/SettingsLayerDetail.h"
#include "App/UserConfig.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "UI/DialogMetrics.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#ifndef _WIN32
#include "Core/VideoBackend.h" // Wayland checks; the custom title bar toggle is Linux-only
#endif

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

namespace App
{

// Import Detail types and functions into this translation unit
using Detail::ComboState;
using Detail::FONT_SIZE_OPTIONS;
using Detail::HISTORY_OPTIONS;
using Detail::optionIndexOf;
using Detail::pickedOption;
using Detail::REFRESH_RATE_OPTIONS;

namespace
{

// Button labels. The dialog's width is worked out from these before the buttons are drawn (see the
// combo sizing in renderSettingsDialog()), so each is named once and used for both.
constexpr const char* EDIT_CONFIG_LABEL = ICON_FA_FILE_PEN "  Edit Config File";
constexpr const char* OPEN_THEMES_LABEL = ICON_FA_FOLDER "  Open Themes Folder";
constexpr const char* CANCEL_LABEL = "Cancel";
// "Save", not "Apply": the button writes config.toml and closes the dialog, which is what Save
// means; "Apply" suggested the dialog would stay open (#1273).
constexpr const char* SAVE_LABEL = "Save";
// Fills the dialog's controls with the defaults; nothing is written until Save.
constexpr const char* RESET_LABEL = "Reset to defaults";
constexpr const char* PRIVILEGE_NOTICE_LABEL = "Show limited-data notice";

// Row labels. Sentence case, like the rest of the dialog's text; the section headers are Title Case.
constexpr const char* THEME_LABEL = "Theme";
constexpr const char* FONT_SIZE_LABEL = "Font size";
constexpr const char* REFRESH_LABEL = "Update interval";
constexpr const char* HISTORY_LABEL = "History length";
#ifndef _WIN32
constexpr const char* NATIVE_DECORATIONS_LABEL = "Use native window decorations instead of the custom title bar";
#endif

// The user themes directory: the one UILayer scans and merges over the built-ins. The built-in
// themes live in the install's assets directory, which is read-only and replaced on upgrade, so
// that is not where users should add themes (#1127). Created on first use so the button always
// opens something.
[[nodiscard]] auto getUserThemesDir() -> std::filesystem::path
{
    auto dir = Core::Application::get().paths().userConfigDir() / "themes";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        spdlog::warn("Couldn't create user themes directory {}: {}", dir.string(), ec.message());
    }
    return dir;
}

} // namespace

SettingsLayer::SettingsLayer() : Core::Layer("SettingsLayer")
{}

SettingsLayer::~SettingsLayer() = default;

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

    // Each combo starts on the stored value, or on none when the stored value isn't an option
    // (an interval set in config.toml, a theme file that's gone); nothing is touched yet.
    const auto themeIt = std::ranges::find(m_Themes, settings.themeId, &UI::DiscoveredTheme::id);
    m_ThemeChoice = ComboState{.index = (themeIt != m_Themes.end()) ? std::optional{static_cast<std::size_t>(themeIt - m_Themes.begin())}
                                                                    : std::nullopt};
    m_FontSizeChoice = ComboState{.index = optionIndexOf(FONT_SIZE_OPTIONS, settings.fontSize, &Detail::FontSizeOption::value)};
    m_RefreshRateChoice =
        ComboState{.index = optionIndexOf(REFRESH_RATE_OPTIONS, settings.refreshIntervalMs, &Detail::RefreshRateOption::valueMs)};
    m_HistoryChoice = ComboState{.index = optionIndexOf(HISTORY_OPTIONS, settings.maxHistorySeconds, &Detail::HistoryOption::valueSeconds)};

    m_CustomThemePreview = std::format("Custom ({})", settings.themeId);
    m_CustomRefreshPreview = Detail::customRefreshLabel(settings.refreshIntervalMs);
    m_CustomHistoryPreview = Detail::customHistoryLabel(settings.maxHistorySeconds);
    m_ForceNativeDecorationsOnWayland = settings.forceNativeWindowDecorationsOnWayland;
    m_ShowPrivilegeNotice = settings.showPrivilegeNotice;
}

void SettingsLayer::resetToDefaults()
{
    // Every control moves to its default and counts as picked, so Save writes it; Cancel still
    // leaves the stored settings as they were (#1273).
    const UserSettings defaults;
    const auto themeIt = std::ranges::find(m_Themes, defaults.themeId, &UI::DiscoveredTheme::id);
    m_ThemeChoice = (themeIt != m_Themes.end()) ? ComboState{.index = static_cast<std::size_t>(themeIt - m_Themes.begin()), .touched = true}
                                                : m_ThemeChoice; // The default theme's file is missing: leave the choice as it is
    m_FontSizeChoice = Detail::defaultChoice(FONT_SIZE_OPTIONS, defaults.fontSize, &Detail::FontSizeOption::value);
    m_RefreshRateChoice = Detail::defaultChoice(REFRESH_RATE_OPTIONS, defaults.refreshIntervalMs, &Detail::RefreshRateOption::valueMs);
    m_HistoryChoice = Detail::defaultChoice(HISTORY_OPTIONS, defaults.maxHistorySeconds, &Detail::HistoryOption::valueSeconds);
    m_ForceNativeDecorationsOnWayland = defaults.forceNativeWindowDecorationsOnWayland;
    m_ShowPrivilegeNotice = defaults.showPrivilegeNotice;
}

void SettingsLayer::applySettings()
{
    auto& config = UserConfig::get();
    auto& settings = config.settings();
    auto& themeManager = UI::Theme::get();

    // Only the combos the user picked are written (#1120, #1151): see Detail::pickedOption.

    // Apply theme
    if (m_ThemeChoice.touched && m_ThemeChoice.index.has_value() && *m_ThemeChoice.index < m_Themes.size())
    {
        const std::string& newThemeId = m_Themes[*m_ThemeChoice.index].id;
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
    if (const auto font = pickedOption(m_FontSizeChoice, FONT_SIZE_OPTIONS); font.has_value() && font->value != settings.fontSize)
    {
        changeFontSize(font->value);
        spdlog::info("Font size changed to {}", font->label);
    }

    // Apply refresh rate
    if (const auto refresh = pickedOption(m_RefreshRateChoice, REFRESH_RATE_OPTIONS);
        refresh.has_value() && refresh->valueMs != settings.refreshIntervalMs)
    {
        const int newRefreshMs = refresh->valueMs;
        settings.refreshIntervalMs = newRefreshMs;
        // Notify panels/samplers of interval change via event
        {
            Core::RefreshRateChangedEvent event(newRefreshMs);
            Core::Application::get().raiseEvent(event);
        }
        spdlog::info("Settings: Refresh rate changed to {} ms", newRefreshMs);
    }

    // Apply history duration
    if (const auto history = pickedOption(m_HistoryChoice, HISTORY_OPTIONS);
        history.has_value() && history->valueSeconds != settings.maxHistorySeconds)
    {
        const int newHistorySeconds = history->valueSeconds;
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

    // Limited-data notice preference (read at startup, like the dialog's own "Don't show again")
    if (m_ShowPrivilegeNotice != settings.showPrivilegeNotice)
    {
        settings.showPrivilegeNotice = m_ShowPrivilegeNotice;
        spdlog::info("Settings: Show limited-data notice changed to {}", m_ShowPrivilegeNotice);
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

    // Kept centred and within the viewport on every frame it is open, not only when it appears: its
    // height grows with the font preset and display scale, and the main window can shrink while it
    // is open. It is NoMove, so re-centring never fights the user. The size cap stops it outgrowing
    // the viewport; the scrolling body below keeps the buttons inside it (#1129).
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 dialogMaxSize(UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x),
                               UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.y));
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0F, 0.0F), dialogMaxSize);
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

        // Escape cancels, as the Cancel button does -- unless one of the combos was open. With
        // keyboard navigation on (UILayer), ImGui's own Escape handling in NewFrame() closes that
        // combo and hands focus back to this dialog within the same frame, so the same key press
        // would otherwise close the combo and then the whole dialog.
        if (!m_ComboOpenLastFrame && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        bool comboOpen = false;

        // Everything above the Cancel/Save row scrolls in a child sized to its content, but never
        // taller than leaves room for that row below it, so the buttons stay on screen at any font
        // size in any window height (#1129). Reserved: the title bar, the window padding, and the
        // footer (separator, spacing and the button row) with the item spacing between them.
        const float footerHeight = (style.ItemSpacing.y * 3.0F) + 1.0F + ImGui::GetFrameHeight();
        const float reservedHeight = ImGui::GetFrameHeight() + (style.WindowPadding.y * 2.0F) + footerHeight;
        const float bodyMaxHeight =
            UI::DialogMetrics::computeScrollableBodyMaxHeight(dialogMaxSize.y, reservedHeight, ImGui::GetFrameHeightWithSpacing() * 2.0F);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0F, 0.0F), ImVec2(std::numeric_limits<float>::max(), bodyMaxHeight));
        ImGui::BeginChild("##SettingsBody", ImVec2(0.0F, 0.0F), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY);

        // ========================================
        // Appearance Section
        // ========================================
        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_PALETTE "  Appearance");
        ImGui::Separator();
        ImGui::Spacing();

        // Column geometry, measured from the text it has to hold rather than fixed at 150/250px.
        // Those constants only looked right at one font size: at Small the labels used a fraction of
        // the 150px column and the 250px combos dwarfed values like "Small" and "250 ms", while at
        // Even Huger 150px was barely enough for "Metric Refresh Rate", as the update interval was
        // labelled then (#921).
        //
        // Measuring is better than an em multiple here because these columns hold variable text: it
        // is self-documenting, it tracks the theme list and option arrays if either gains an entry,
        // and it absorbs the glyph-metric differences between platforms automatically.
        const float emPx = ImGui::GetFontSize();
        const float labelGap = style.ItemSpacing.x * 2.0F;
        const float widestLabel = std::max({
            ImGui::CalcTextSize(THEME_LABEL).x,
            ImGui::CalcTextSize(FONT_SIZE_LABEL).x,
            ImGui::CalcTextSize(REFRESH_LABEL).x,
            ImGui::CalcTextSize(HISTORY_LABEL).x,
        });
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
        if (!m_ThemeChoice.index.has_value())
        {
            widestAppearanceValue = std::max(widestAppearanceValue, ImGui::CalcTextSize(m_CustomThemePreview.c_str()).x);
        }
        // Capped against the viewport. Theme names are read from a user's TOML with no length limit
        // (ThemeLoader), so measuring them is unbounded: a long name would otherwise widen this
        // auto-resizing popup past the window and put the combo's arrow and the buttons below it out
        // of reach. The floor keeps the control usable if the cap bites; ImGui clips the combo's
        // preview text, so a long name degrades to truncation rather than an unreachable control.
        const float comboMinWidth = (MIN_COMBO_EM * emPx) + comboDecoration;

        // The dialog auto-fits its widest row, and that is usually not a combo row: the two
        // Advanced buttons and the Reset/Cancel/Save row are both wider. Sized only to their own text,
        // the combos stopped short of the dialog's right edge, in line with neither the separators
        // nor Save (#972). Those rows are measured from text as well, so the width they will give
        // the dialog is known here, and the combos are widened to reach it.
        const float advancedRowWidth = ImGui::CalcTextSize(EDIT_CONFIG_LABEL).x + ImGui::CalcTextSize(OPEN_THEMES_LABEL).x +
                                       (style.FramePadding.x * 4.0F) + style.ItemSpacing.x;
        const float actionButtonWidth =
            std::max(UI::DialogMetrics::computeActionButtonWidth(ImGui::CalcTextSize(CANCEL_LABEL).x, emPx, SETTINGS_BUTTON_MIN_EM),
                     UI::DialogMetrics::computeActionButtonWidth(ImGui::CalcTextSize(SAVE_LABEL).x, emPx, SETTINGS_BUTTON_MIN_EM));
        // Reset to defaults sits at the left of the same row, at its own text's width.
        const float resetButtonWidth = ImGui::CalcTextSize(RESET_LABEL).x + (style.FramePadding.x * 2.0F);
        const float actionRowWidth = resetButtonWidth + (actionButtonWidth * 2.0F) + (style.ItemSpacing.x * 2.0F);
        // The Advanced section's checkboxes: a square of the frame height, then the label. On
        // native Wayland the window-decorations one is the widest row in the dialog.
        const auto checkboxWidth = [&style](const char* label)
        {
            return ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
        };
#ifndef _WIN32
        const float checkboxRowWidth = std::max(checkboxWidth(PRIVILEGE_NOTICE_LABEL),
                                                Core::VideoBackend::isWayland() ? checkboxWidth(NATIVE_DECORATIONS_LABEL) : 0.0F);
#else
        const float checkboxRowWidth = checkboxWidth(PRIVILEGE_NOTICE_LABEL);
#endif
        const float widestOtherRow = std::max({advancedRowWidth, actionRowWidth, checkboxRowWidth});

        // The rows are laid out inside the scrolling body, which has no padding of its own: its
        // content starts at 0, flush with the dialog's content edge. The row budget allows for the
        // dialog's padding either side plus the body's scrollbar, which appears when it scrolls.
        const float rowSurrounding = (style.WindowPadding.x * 2.0F) + style.ScrollbarSize;
        const float appearanceComboWidth = UI::DialogMetrics::computeCappedControlWidth(
            UI::DialogMetrics::computeFilledControlWidth(widestAppearanceValue + comboDecoration, valueColumn, 0.0F, widestOtherRow),
            valueColumn,
            rowSurrounding,
            viewport->WorkSize.x,
            comboMinWidth);

        // Theme dropdown
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(THEME_LABEL);
        ImGui::SameLine(valueColumn);
        ImGui::SetNextItemWidth(appearanceComboWidth);

        if (!m_Themes.empty())
        {
            const char* currentTheme = (m_ThemeChoice.index.has_value() && *m_ThemeChoice.index < m_Themes.size())
                                         ? m_Themes[*m_ThemeChoice.index].name.c_str()
                                         : m_CustomThemePreview.c_str();
            if (ImGui::BeginCombo("##Theme", currentTheme))
            {
                comboOpen = true;
                for (std::size_t i = 0; i < m_Themes.size(); ++i)
                {
                    const bool isSelected = (m_ThemeChoice.index == i);
                    // Scoped by id: two themes can share a display name (a user copy of a built-in,
                    // renamed), and the label alone would give both entries one widget ID.
                    ImGui::PushID(m_Themes[i].id.c_str());
                    if (ImGui::Selectable(m_Themes[i].name.c_str(), isSelected))
                    {
                        m_ThemeChoice = ComboState{.index = i, .touched = true};
                    }
                    if (isSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }

        ImGui::Spacing();

        // Font size dropdown
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(FONT_SIZE_LABEL);
        ImGui::SameLine(valueColumn);
        ImGui::SetNextItemWidth(appearanceComboWidth);

        // NOLINT comments below: label is always initialized from a string literal, so .data() is null-terminated
        // The font size always matches an option (the enum has no other values), but a Ctrl+= change
        // while the dialog is open moves it, so the preview follows the live setting.
        const auto liveFontIndex = optionIndexOf(FONT_SIZE_OPTIONS, UserConfig::get().settings().fontSize, &Detail::FontSizeOption::value);
        const std::size_t fontPreviewIndex = (m_FontSizeChoice.touched ? m_FontSizeChoice.index : liveFontIndex).value_or(1);
        const char* currentFontSize = FONT_SIZE_OPTIONS[fontPreviewIndex].label.data(); // NOLINT(bugprone-suspicious-stringview-data-usage)
        if (ImGui::BeginCombo("##FontSize", currentFontSize))
        {
            comboOpen = true;
            for (std::size_t i = 0; i < FONT_SIZE_OPTIONS.size(); ++i)
            {
                const bool isSelected = (fontPreviewIndex == i);
                if (ImGui::Selectable(FONT_SIZE_OPTIONS[i].label.data(), isSelected)) // NOLINT(bugprone-suspicious-stringview-data-usage)
                {
                    m_FontSizeChoice = ComboState{.index = i, .touched = true};
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
        // Performance Section
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
        // A stored value that isn't an option shows as "Custom (...)", which can be the widest.
        if (!m_RefreshRateChoice.index.has_value())
        {
            widestPerfValue = std::max(widestPerfValue, ImGui::CalcTextSize(m_CustomRefreshPreview.c_str()).x);
        }
        if (!m_HistoryChoice.index.has_value())
        {
            widestPerfValue = std::max(widestPerfValue, ImGui::CalcTextSize(m_CustomHistoryPreview.c_str()).x);
        }
        const float perfComboWidth = UI::DialogMetrics::computeCappedControlWidth(
            widestPerfValue + comboDecoration, valueColumn, rowSurrounding, viewport->WorkSize.x, comboMinWidth);
        const float perfLabelWidth = UI::DialogMetrics::computeRightAlignedStart(valueColumn, appearanceComboWidth, perfComboWidth);

        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_GAUGE_HIGH "  Performance");
        ImGui::Separator();
        ImGui::Spacing();

        // Update interval dropdown
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(REFRESH_LABEL);
        ImGui::SameLine(perfLabelWidth);
        ImGui::SetNextItemWidth(perfComboWidth);

        const char* currentRefresh =
            m_RefreshRateChoice.index.has_value()
                ? REFRESH_RATE_OPTIONS[*m_RefreshRateChoice.index].label.data() // NOLINT(bugprone-suspicious-stringview-data-usage)
                : m_CustomRefreshPreview.c_str();
        if (ImGui::BeginCombo("##RefreshRate", currentRefresh))
        {
            comboOpen = true;
            for (std::size_t i = 0; i < REFRESH_RATE_OPTIONS.size(); ++i)
            {
                const bool isSelected = (m_RefreshRateChoice.index == i);
                if (ImGui::Selectable(REFRESH_RATE_OPTIONS[i].label.data(), // NOLINT(bugprone-suspicious-stringview-data-usage)
                                      isSelected))
                {
                    m_RefreshRateChoice = ComboState{.index = i, .touched = true};
                }
                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();

        // History length dropdown
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(HISTORY_LABEL);
        ImGui::SameLine(perfLabelWidth);
        ImGui::SetNextItemWidth(perfComboWidth);

        const char* currentHistory =
            m_HistoryChoice.index.has_value()
                ? HISTORY_OPTIONS[*m_HistoryChoice.index].label.data() // NOLINT(bugprone-suspicious-stringview-data-usage)
                : m_CustomHistoryPreview.c_str();
        if (ImGui::BeginCombo("##History", currentHistory))
        {
            comboOpen = true;
            for (std::size_t i = 0; i < HISTORY_OPTIONS.size(); ++i)
            {
                const bool isSelected = (m_HistoryChoice.index == i);
                if (ImGui::Selectable(HISTORY_OPTIONS[i].label.data(), isSelected)) // NOLINT(bugprone-suspicious-stringview-data-usage)
                {
                    m_HistoryChoice = ComboState{.index = i, .touched = true};
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
        // Advanced Section
        // ========================================
        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_FOLDER_OPEN "  Advanced");
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
            (void) App::PlatformOpen::openWithSystemHandler(getUserThemesDir());
        }
        ImGui::PopStyleColor();

        // The notice TaskSmack shows at startup when it runs without the rights to read every
        // process. Its "Don't show again" clears this; here it can be turned back on (#1273).
        ImGui::Spacing();
        ImGui::Checkbox(PRIVILEGE_NOTICE_LABEL, &m_ShowPrivilegeNotice);
        ImGui::SetItemTooltip("At startup, say when TaskSmack can't read every process's details without administrator or root rights");

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

        ImGui::EndChild(); // ##SettingsBody

        ImGui::Separator();
        ImGui::Spacing();

        // ========================================
        // Buttons (pinned below the scrolling body)
        // ========================================
        // Floor of 9.375 em is exactly the former fixed 100px at the reference configuration; the
        // measured term takes over for whichever of the two labels is wider once the font grows.
        // (Computed above as actionButtonWidth, where the combos need it to find the dialog's width.)
        // Shrunk to the row when the viewport-capped dialog is narrower than the row (#1129), so
        // Cancel can't be pushed off the left edge. Reset to defaults keeps its width at the left.
        const float rowStartX = ImGui::GetCursorPosX();
        const float availWidth = ImGui::GetContentRegionAvail().x;
        const float pairAvailWidth = std::max(0.0F, availWidth - resetButtonWidth - style.ItemSpacing.x);
        const float buttonWidth = UI::DialogMetrics::fitActionButtonPairWidth(actionButtonWidth, style.ItemSpacing.x, pairAvailWidth);
        const float totalButtonWidth = (buttonWidth * 2.0F) + style.ItemSpacing.x;

        // Push text color to ensure visibility on button backgrounds
        ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);
        if (ImGui::Button(RESET_LABEL, ImVec2(resetButtonWidth, 0.0F)))
        {
            resetToDefaults();
        }
        ImGui::SetItemTooltip("Put every setting here back to its default; Save keeps them");
        ImGui::SameLine();

        // Right-align Cancel and Save
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rowStartX + availWidth - totalButtonWidth));

        if (ImGui::Button(CANCEL_LABEL, ImVec2(buttonWidth, 0.0F)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        // Save button with success color for positive action. Its label is drawn in whichever of
        // the theme's two poles -- its text colour or its window background -- reads better on the
        // fill showing in the button's current state; the ordinary text colour was nearly invisible
        // on it in most of the bundled themes (#969).
        if (UI::Widgets::filledButton(SAVE_LABEL,
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

        m_ComboOpenLastFrame = comboOpen;
        ImGui::EndPopup();
    }
    else
    {
        m_ComboOpenLastFrame = false;
    }
}

} // namespace App
