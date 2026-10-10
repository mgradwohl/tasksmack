#include "SettingsDialog.h"

#include "App/DialogGeometry.h"
#include "App/SettingsLayerDetail.h"
#include "App/UserConfig.h"
#include "UI/ChromeLayout.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#ifndef _WIN32
#include "Core/VideoBackend.h" // Wayland checks; the custom title bar toggle is Linux-only
#endif

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace App::SettingsDialog
{

using Detail::ComboState;
using Detail::FONT_SIZE_OPTIONS;
using Detail::HISTORY_OPTIONS;
using Detail::optionIndexOf;
using Detail::REFRESH_RATE_OPTIONS;

namespace
{

// Row labels. Sentence case, like the rest of the dialog's text; the section headers are Title Case.
constexpr const char* THEME_LABEL = "Theme";
constexpr const char* FONT_SIZE_LABEL = "Font size";
constexpr const char* REFRESH_LABEL = "Update interval";
constexpr const char* HISTORY_LABEL = "History length";
#ifndef _WIN32
constexpr const char* NATIVE_DECORATIONS_LABEL = "Use native window decorations instead of the custom title bar";
#endif

} // namespace

void load(State& state, const UserSettings& settings, std::vector<UI::DiscoveredTheme> themes)
{
    state.themes = std::move(themes);

    // Each combo starts on the stored value, or on none when the stored value isn't an option
    // (an interval set in config.toml, a theme file that's gone); nothing is touched yet.
    const auto themeIt = std::ranges::find(state.themes, settings.themeId, &UI::DiscoveredTheme::id);
    state.themeChoice = ComboState{
        .index = (themeIt != state.themes.end()) ? std::optional{static_cast<std::size_t>(themeIt - state.themes.begin())} : std::nullopt};
    state.fontSizeChoice = ComboState{.index = optionIndexOf(FONT_SIZE_OPTIONS, settings.fontSize, &Detail::FontSizeOption::value)};
    state.refreshRateChoice =
        ComboState{.index = optionIndexOf(REFRESH_RATE_OPTIONS, settings.refreshIntervalMs, &Detail::RefreshRateOption::valueMs)};
    state.historyChoice =
        ComboState{.index = optionIndexOf(HISTORY_OPTIONS, settings.maxHistorySeconds, &Detail::HistoryOption::valueSeconds)};

    state.customThemePreview = std::format("Custom ({})", settings.themeId);
    state.customRefreshPreview = Detail::customRefreshLabel(settings.refreshIntervalMs);
    state.customHistoryPreview = Detail::customHistoryLabel(settings.maxHistorySeconds);
    state.forceNativeDecorationsOnWayland = settings.forceNativeWindowDecorationsOnWayland;
    state.showPrivilegeNotice = settings.showPrivilegeNotice;
}

void resetToDefaults(State& state)
{
    // Every control moves to its default and counts as picked, so Save writes it; Cancel still
    // leaves the stored settings as they were (#1273).
    const UserSettings defaults;
    const auto themeIt = std::ranges::find(state.themes, defaults.themeId, &UI::DiscoveredTheme::id);
    state.themeChoice = (themeIt != state.themes.end())
                          ? ComboState{.index = static_cast<std::size_t>(themeIt - state.themes.begin()), .touched = true}
                          : state.themeChoice; // The default theme's file is missing: leave the choice as it is
    state.fontSizeChoice = Detail::defaultChoice(FONT_SIZE_OPTIONS, defaults.fontSize, &Detail::FontSizeOption::value);
    state.refreshRateChoice = Detail::defaultChoice(REFRESH_RATE_OPTIONS, defaults.refreshIntervalMs, &Detail::RefreshRateOption::valueMs);
    state.historyChoice = Detail::defaultChoice(HISTORY_OPTIONS, defaults.maxHistorySeconds, &Detail::HistoryOption::valueSeconds);
    state.forceNativeDecorationsOnWayland = defaults.forceNativeWindowDecorationsOnWayland;
    state.showPrivilegeNotice = defaults.showPrivilegeNotice;
}

Action render(State& state)
{
    const bool isOpen = ImGui::IsPopupOpen(POPUP_ID);
    if (!state.openRequested && !isOpen)
    {
        return Action::None;
    }

    if (state.openRequested)
    {
        ImGui::OpenPopup(POPUP_ID);
        state.openRequested = false;
    }

    // Kept centred and within the viewport on every frame it is open, not only when it appears: its
    // height grows with the font preset and display scale, and the main window can shrink while it
    // is open. It is NoMove, so re-centring never fights the user. The size cap stops it outgrowing
    // the viewport; the scrolling body below keeps the buttons inside it (#1129).
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 dialogMaxSize(UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x),
                               UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.y));
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    UI::Widgets::setNextDialogSizeConstraints(dialogMaxSize);
    // No explicit width. The former fixed 450px is gone and nothing replaces it: the popup is
    // ImGuiWindowFlags_AlwaysAutoResize and every column below is measured from the text it has to
    // hold, so auto-fit already produces exactly the width the content needs at the current font.
    //
    // Deliberately not re-expressed as an em multiple. ImGui honours SetNextWindowSize over
    // AlwaysAutoResize only on frames where the size was genuinely set by the API, so an
    // ImGuiCond_Appearing width is discarded by auto-fit from the second frame on -- it would be
    // inert code that merely looked like it was doing something. See #947's review of the same
    // pattern in ElevationNoticeLayer, where the width is authored and so is reapplied every frame.

    Action action = Action::None;
    const ImGuiWindowFlags popupFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove;

    if (ImGui::BeginPopupModal(POPUP_ID, nullptr, popupFlags))
    {
        const auto& theme = UI::Theme::get();
        const ImGuiStyle& style = ImGui::GetStyle();

        // Escape cancels, as the Cancel button does -- unless one of the combos was open. With
        // keyboard navigation on (UILayer), ImGui's own Escape handling in NewFrame() closes that
        // combo and hands focus back to this dialog within the same frame, so the same key press
        // would otherwise close the combo and then the whole dialog.
        if (!state.comboOpenLastFrame && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        bool comboOpen = false;

        // Everything above the Cancel/Save row scrolls in a child sized to its content, but never
        // taller than leaves room for that row below it, so the buttons stay on screen at any font
        // size in any window height (#1129). Reserved: the title bar, the window padding, and the
        // footer (separator, spacing and the button row) with the item spacing between them.
        // The action row's buttons, measured from their text: the floor of 9.375 em is exactly the
        // former fixed 100px at the reference configuration, and the measured term takes over for
        // whichever of Cancel and Save is wider once the font grows. Reset to defaults sits at their
        // left at its own text's width, while the three fit at full width; in a dialog narrower than
        // that (the 200 px minimum window at the largest font) it takes a row of its own above them,
        // so Cancel and Save are never pushed past the dialog's edge (#1341 review).
        // The footer is the one every dialog shares (UI::Widgets::dialogFooter(), #1200); it is laid
        // out here, before the body, with the same inputs it will see, so the height reserved for it
        // is the height it takes.
        const float actionButtonWidth = UI::Widgets::footerButtonWidth({CANCEL_LABEL, SAVE_LABEL}, SETTINGS_BUTTON_MIN_EM);
        const float resetButtonWidth = UI::Widgets::footerLeadingButtonWidth(RESET_LABEL);
        const float actionRowWidth = resetButtonWidth + (actionButtonWidth * 2.0F) + (style.ItemSpacing.x * 2.0F);
        // The item spacing after the body, then the footer itself, laid out from the inputs
        // dialogFooter() will see below.
        const float footerHeight =
            style.ItemSpacing.y + UI::ChromeLayout::layoutDialogFooter(UI::Widgets::dialogFooterInput(actionButtonWidth, true, RESET_LABEL),
                                                                       style.ItemSpacing.y,
                                                                       ImGui::GetFrameHeight())
                                      .height;
        const float reservedHeight = ImGui::GetFrameHeight() + (style.WindowPadding.y * 2.0F) + footerHeight;
        const float bodyMaxHeight =
            UI::DialogMetrics::computeScrollableBodyMaxHeight(dialogMaxSize.y, reservedHeight, ImGui::GetFrameHeightWithSpacing() * 2.0F);
        UI::Widgets::setNextDialogSizeConstraints(ImVec2(std::numeric_limits<float>::max(), bodyMaxHeight));
        ImGui::BeginChild("##SettingsBody", ImVec2(0.0F, 0.0F), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY);

        // ========================================
        // Appearance Section
        // ========================================
        (void) UI::Widgets::sectionHeader(ICON_FA_PALETTE, "Appearance");
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
        for (const auto& themeEntry : state.themes)
        {
            widestAppearanceValue = std::max(widestAppearanceValue, ImGui::CalcTextSize(themeEntry.name.c_str()).x);
        }
        for (const auto& option : FONT_SIZE_OPTIONS)
        {
            widestAppearanceValue =
                std::max(widestAppearanceValue, ImGui::CalcTextSize(option.label.data(), option.label.data() + option.label.size()).x);
        }
        if (!state.themeChoice.index.has_value())
        {
            widestAppearanceValue = std::max(widestAppearanceValue, ImGui::CalcTextSize(state.customThemePreview.c_str()).x);
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
        // actionButtonWidth, resetButtonWidth and actionRowWidth are measured above, with the footer.
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

        if (!state.themes.empty())
        {
            const char* currentTheme = (state.themeChoice.index.has_value() && *state.themeChoice.index < state.themes.size())
                                         ? state.themes[*state.themeChoice.index].name.c_str()
                                         : state.customThemePreview.c_str();
            if (ImGui::BeginCombo("##Theme", currentTheme))
            {
                comboOpen = true;
                for (std::size_t i = 0; i < state.themes.size(); ++i)
                {
                    const bool isSelected = (state.themeChoice.index == i);
                    // Scoped by id: two themes can share a display name (a user copy of a built-in,
                    // renamed), and the label alone would give both entries one widget ID.
                    ImGui::PushID(state.themes[i].id.c_str());
                    if (ImGui::Selectable(state.themes[i].name.c_str(), isSelected))
                    {
                        state.themeChoice = ComboState{.index = i, .touched = true};
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
        const std::size_t fontPreviewIndex = (state.fontSizeChoice.touched ? state.fontSizeChoice.index : liveFontIndex).value_or(1);
        const char* currentFontSize = FONT_SIZE_OPTIONS[fontPreviewIndex].label.data(); // NOLINT(bugprone-suspicious-stringview-data-usage)
        if (ImGui::BeginCombo("##FontSize", currentFontSize))
        {
            comboOpen = true;
            for (std::size_t i = 0; i < FONT_SIZE_OPTIONS.size(); ++i)
            {
                const bool isSelected = (fontPreviewIndex == i);
                if (ImGui::Selectable(FONT_SIZE_OPTIONS[i].label.data(), isSelected)) // NOLINT(bugprone-suspicious-stringview-data-usage)
                {
                    state.fontSizeChoice = ComboState{.index = i, .touched = true};
                }
                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        UI::Widgets::sectionGap();

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
        if (!state.refreshRateChoice.index.has_value())
        {
            widestPerfValue = std::max(widestPerfValue, ImGui::CalcTextSize(state.customRefreshPreview.c_str()).x);
        }
        if (!state.historyChoice.index.has_value())
        {
            widestPerfValue = std::max(widestPerfValue, ImGui::CalcTextSize(state.customHistoryPreview.c_str()).x);
        }
        const float perfComboWidth = UI::DialogMetrics::computeCappedControlWidth(
            widestPerfValue + comboDecoration, valueColumn, rowSurrounding, viewport->WorkSize.x, comboMinWidth);
        const float perfLabelWidth = UI::DialogMetrics::computeRightAlignedStart(valueColumn, appearanceComboWidth, perfComboWidth);

        (void) UI::Widgets::sectionHeader(ICON_FA_GAUGE_HIGH, "Performance");
        ImGui::Separator();
        ImGui::Spacing();

        // Update interval dropdown
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(REFRESH_LABEL);
        ImGui::SameLine(perfLabelWidth);
        ImGui::SetNextItemWidth(perfComboWidth);

        const char* currentRefresh =
            state.refreshRateChoice.index.has_value()
                ? REFRESH_RATE_OPTIONS[*state.refreshRateChoice.index].label.data() // NOLINT(bugprone-suspicious-stringview-data-usage)
                : state.customRefreshPreview.c_str();
        if (ImGui::BeginCombo("##RefreshRate", currentRefresh))
        {
            comboOpen = true;
            for (std::size_t i = 0; i < REFRESH_RATE_OPTIONS.size(); ++i)
            {
                const bool isSelected = (state.refreshRateChoice.index == i);
                if (ImGui::Selectable(REFRESH_RATE_OPTIONS[i].label.data(), // NOLINT(bugprone-suspicious-stringview-data-usage)
                                      isSelected))
                {
                    state.refreshRateChoice = ComboState{.index = i, .touched = true};
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
            state.historyChoice.index.has_value()
                ? HISTORY_OPTIONS[*state.historyChoice.index].label.data() // NOLINT(bugprone-suspicious-stringview-data-usage)
                : state.customHistoryPreview.c_str();
        if (ImGui::BeginCombo("##History", currentHistory))
        {
            comboOpen = true;
            for (std::size_t i = 0; i < HISTORY_OPTIONS.size(); ++i)
            {
                const bool isSelected = (state.historyChoice.index == i);
                if (ImGui::Selectable(HISTORY_OPTIONS[i].label.data(), isSelected)) // NOLINT(bugprone-suspicious-stringview-data-usage)
                {
                    state.historyChoice = ComboState{.index = i, .touched = true};
                }
                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        UI::Widgets::sectionGap();

        // ========================================
        // Advanced Section
        // ========================================
        (void) UI::Widgets::sectionHeader(ICON_FA_FOLDER_OPEN, "Advanced");
        ImGui::Separator();
        ImGui::Spacing();

        // Button row for config file and themes folder
        // Push text color to ensure visibility on button backgrounds
        ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);
        if (ImGui::Button(EDIT_CONFIG_LABEL))
        {
            action = Action::EditConfig;
        }
        ImGui::SameLine();
        if (ImGui::Button(OPEN_THEMES_LABEL))
        {
            action = Action::OpenThemesFolder;
        }
        ImGui::PopStyleColor();

        // The notice TaskSmack shows at startup when it runs without the rights to read every
        // process. Its "Don't show again" clears this; here it can be turned back on (#1273).
        ImGui::Spacing();
        ImGui::Checkbox(PRIVILEGE_NOTICE_LABEL, &state.showPrivilegeNotice);
        ImGui::SetItemTooltip("At startup, say when TaskSmack can't read every process's details without administrator or root rights");

        // About moved out of F1, which now opens Help (#172); it is reached from here and from Help.
        // Only one modal is open at a time, so this closes Settings, as Cancel does, first.
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);
        if (ImGui::Button(ABOUT_LABEL))
        {
            ImGui::CloseCurrentPopup();
            action = Action::OpenAbout;
        }
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Version, build and licences. Closes Settings without saving.");

#ifndef _WIN32
        // Only meaningful on native Wayland -- the custom title bar's drag/resize
        // implementation depends on compositor hand-off there in ways that don't apply
        // to X11/XWayland (see #744, #749). Hidden elsewhere since it would have no effect.
        if (Core::VideoBackend::isWayland())
        {
            ImGui::Spacing();
            ImGui::Checkbox(NATIVE_DECORATIONS_LABEL, &state.forceNativeDecorationsOnWayland);
            ImGui::TextColored(theme.scheme().textMuted, "Takes effect after restarting TaskSmack.");
        }
#endif

        ImGui::EndChild(); // ##SettingsBody

        // ========================================
        // Buttons (pinned below the scrolling body)
        // ========================================
        // The footer every dialog shares (#1200): [Reset to defaults] ... [Cancel][Save]. Cancel and
        // Save at actionButtonWidth (measured above, with the footer's height), shrunk to the row when
        // the viewport-capped dialog is narrower than it (#1129), so Cancel can't be pushed off the
        // left edge. Reset to defaults keeps its width: at their left, or on its own row above them
        // when the dialog is too narrow for all three -- decided from the same inputs the reserved
        // height above was measured with.
        //
        // Save fills with the success colour for the positive action. Its label is drawn in whichever
        // of the theme's two poles -- its text colour or its window background -- reads better on the
        // fill showing in the button's current state; the ordinary text colour was nearly invisible
        // on it in most of the bundled themes (#969).
        const UI::Widgets::ButtonFills saveFills{
            .resting = theme.scheme().successButton,
            .hovered = theme.scheme().successButtonHovered,
            .pressed = theme.scheme().successButtonActive,
        };
        const UI::Widgets::DialogFooterButton saveButton{.label = SAVE_LABEL, .fills = &saveFills, .tooltip = nullptr};
        const UI::Widgets::DialogFooterButton cancelButton{.label = CANCEL_LABEL, .fills = nullptr, .tooltip = nullptr};
        const UI::Widgets::DialogFooterButton resetButton{
            .label = RESET_LABEL, .fills = nullptr, .tooltip = "Put every setting here back to its default; Save keeps them"};
        switch (UI::Widgets::dialogFooter(saveButton, cancelButton, actionButtonWidth, resetButton))
        {
        case UI::Widgets::DialogFooterAction::Primary:
            action = Action::Save;
            ImGui::CloseCurrentPopup();
            break;
        case UI::Widgets::DialogFooterAction::Secondary:
            ImGui::CloseCurrentPopup();
            break;
        case UI::Widgets::DialogFooterAction::Leading:
            resetToDefaults(state);
            break;
        case UI::Widgets::DialogFooterAction::None:
            break;
        }

        state.comboOpenLastFrame = comboOpen;
        ImGui::EndPopup();
    }
    else
    {
        state.comboOpenLastFrame = false;
    }
    return action;
}

} // namespace App::SettingsDialog
