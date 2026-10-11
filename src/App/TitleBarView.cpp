#include "TitleBarView.h"

#include "App/TitleBarButtons.h"
#include "App/TitleBarGeometry.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

namespace App::TitleBarView
{

namespace
{

// Code points of the title-bar control glyphs, needed to look their ink boxes up in the baked icon
// font. They must stay in step with the matching ICON_FA_* strings in IconsFontAwesome6.h.
constexpr ImWchar CHROME_GLYPH_WINDOW_MINIMIZE = 0xF2D1;
constexpr ImWchar CHROME_GLYPH_XMARK = 0xF00D;

// Draw one chrome glyph over a title-bar button, sized so its ink matches the control glyphs beside
// it and centred on that ink rather than on its text line box.
//
// window-minimize is the reference because it is the one control glyph the bar always shows at the
// same size: it spans the full em of ink width, as window-maximize, window-restore and
// circle-question do, and unlike the maximize button it never swaps glyph with the window state.
// See computeMatchedGlyphSize() for why the match is made on width rather than height.
//
// ImGui::Button centres a label by its line box, which is right only while every glyph sits the
// same way inside its em box. fa-xmark does not, so left as a button label its X comes out both
// smaller and higher than its neighbours. The baked glyph carries its
// ink rectangle in X0/Y0..X1/Y1 relative to the text layout position, so both corrections are read
// from the font itself and neither needs a constant here that a change of icon font would stale.
//
// A no-op when the chrome icon font is missing; the caller falls back to a plain button label.
void drawChromeGlyphMatched(
    ImFont* font, float chromeIconPx, const char* text, ImWchar codepoint, const ImVec2& rectMin, const ImVec2& rectMax)
{
    ImFontBaked* baked = (font != nullptr) ? font->GetFontBaked(chromeIconPx) : nullptr;
    if (baked == nullptr)
    {
        return;
    }
    const ImFontGlyph* reference = baked->FindGlyphNoFallback(CHROME_GLYPH_WINDOW_MINIMIZE);
    const ImFontGlyph* atBaseSize = baked->FindGlyphNoFallback(codepoint);
    if (reference == nullptr || atBaseSize == nullptr)
    {
        return;
    }

    const float matchedPx = computeMatchedGlyphSize(chromeIconPx, reference->X1 - reference->X0, atBaseSize->X1 - atBaseSize->X0);
    ImFontBaked* matchedBaked = font->GetFontBaked(matchedPx);
    const ImFontGlyph* glyph = (matchedBaked != nullptr) ? matchedBaked->FindGlyphNoFallback(codepoint) : nullptr;
    if (glyph == nullptr)
    {
        return;
    }

    const ImVec2 center((rectMin.x + rectMax.x) * 0.5F, (rectMin.y + rectMax.y) * 0.5F);
    const ImVec2 pos(center.x - ((glyph->X0 + glyph->X1) * 0.5F), center.y - ((glyph->Y0 + glyph->Y1) * 0.5F));
    ImGui::GetWindowDrawList()->AddText(font, matchedPx, pos, ImGui::GetColorU32(ImGuiCol_Text), text);
}

/// The Linux system menu, while open: the window command chosen, or None.
[[nodiscard]] Action renderSystemMenu(float titleBarHeight, bool isMaximized)
{
    Action chosen = Action::None;
    // Set position for the popup (below the icon)
    // Under the icon, which sits at this same fraction of the bar height from the left edge.
    ImGui::SetNextWindowPos(ImVec2(titleBarHeight * TITLE_BAR_EDGE_MARGIN_RATIO, titleBarHeight), ImGuiCond_Appearing);

    if (ImGui::BeginPopup(SYSTEM_MENU_ID))
    {
        // Restore (only enabled when maximized)
        if (isMaximized)
        {
            if (ImGui::MenuItem(ICON_FA_WINDOW_RESTORE "  Restore"))
            {
                chosen = Action::Restore;
            }
        }
        else
        {
            ImGui::BeginDisabled();
            ImGui::MenuItem(ICON_FA_WINDOW_RESTORE "  Restore");
            ImGui::EndDisabled();
        }

        // Move (disabled when maximized)
        if (isMaximized)
        {
            ImGui::BeginDisabled();
            ImGui::MenuItem(ICON_FA_ARROW_RIGHT "  Move");
            ImGui::EndDisabled();
        }
        else
        {
            // Move mode not implemented - would require special hit test mode
            static_cast<void>(ImGui::MenuItem(ICON_FA_ARROW_RIGHT "  Move"));
        }

        // Size (disabled when maximized)
        if (isMaximized)
        {
            ImGui::BeginDisabled();
            ImGui::MenuItem(ICON_FA_EXPAND "  Size");
            ImGui::EndDisabled();
        }
        else
        {
            // Size mode not implemented - would require special hit test mode
            static_cast<void>(ImGui::MenuItem(ICON_FA_EXPAND "  Size"));
        }

        // Minimize
        if (ImGui::MenuItem(ICON_FA_WINDOW_MINIMIZE "  Minimize"))
        {
            chosen = Action::Minimize;
        }

        // Maximize (only enabled when not maximized)
        if (!isMaximized)
        {
            if (ImGui::MenuItem(ICON_FA_WINDOW_MAXIMIZE "  Maximize"))
            {
                chosen = Action::Maximize;
            }
        }
        else
        {
            ImGui::BeginDisabled();
            ImGui::MenuItem(ICON_FA_WINDOW_MAXIMIZE "  Maximize");
            ImGui::EndDisabled();
        }

        ImGui::Separator();

        // Close with shortcut hint
        if (ImGui::MenuItem(ICON_FA_XMARK "  Close", "Alt+F4"))
        {
            chosen = Action::Close;
        }

        ImGui::EndPopup();
    }
    return chosen;
}

} // namespace

float iconSize(float barHeight) noexcept
{
    return computeTitleBarIconSize(barHeight, barHeight * TITLE_BAR_ICON_INSET_RATIO);
}

Output render(const Input& input)
{
    const auto& scheme = UI::Theme::get().scheme();
    Output output;
    const float windowWidth = input.windowWidth;
    const float windowHeight = input.windowHeight;
    const float titleBarHeight = input.barHeight;
    // Set up window for title bar - no padding, no scrolling, fixed position
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(windowWidth, titleBarHeight));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    // From the bar's height like the rest of its geometry, so it follows display density but not the
    // Font Size setting: the former fixed 8 x 4 px at the default density (#1200).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(titleBarHeight * TITLE_BAR_PADDING_X_RATIO, titleBarHeight * TITLE_BAR_PADDING_Y_RATIO));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, scheme.titleBgActive);

    ImGui::Begin("##TitleBar", nullptr, flags);

    // Icon (left side) - clickable for system menu
    // Size: title bar height minus 2px border on top and bottom
    const float ICON_SIZE = iconSize(titleBarHeight);
    const float centerY = titleBarHeight * 0.5F;
    const float iconY = centerY - (ICON_SIZE * 0.5F);
    // Left margin and the gap after the icon, proportional to the bar so they hold at any density.
    const float iconX = titleBarHeight * TITLE_BAR_EDGE_MARGIN_RATIO;

    if (input.icon != nullptr && input.icon->valid())
    {
        ImGui::SetCursorPos(ImVec2(iconX, iconY));

        // Make icon clickable with invisible button.
        // We use titleBgActive with zero alpha rather than a literal ImVec4(0,0,0,0) so that
        // if ImGui ever composites the RGB channel even at alpha=0, we still blend with
        // the actual title bar background color rather than black.
        ImGui::PushStyleColor(ImGuiCol_Button, UI::withAlpha(scheme.titleBgActive, 0.0F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, scheme.tabHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, scheme.tabSelected);
        if (ImGui::InvisibleButton("##IconButton", ImVec2(ICON_SIZE, ICON_SIZE)))
        {
            output.action = Action::IconClicked;
#ifndef _WIN32
            // The custom system menu, in the same frame (Windows shows its native one instead).
            ImGui::OpenPopup(SYSTEM_MENU_ID);
#endif
        }
        ImGui::PopStyleColor(3);

        // Draw icon over the invisible button
        ImGui::SetCursorPos(ImVec2(iconX, iconY));
        ImGui::Image(input.icon->textureId(), ImVec2(ICON_SIZE, ICON_SIZE));

        // Track icon bounds for right-click detection
        output.iconBounds = {.minX = iconX, .maxX = iconX + ICON_SIZE, .minY = iconY, .maxY = iconY + ICON_SIZE};
        output.iconDrawn = true;
    }

    // Title text using Sixtyfour font - centered vertically
    ImGui::SameLine();
    ImGui::SetCursorPosX(iconX + ICON_SIZE + (titleBarHeight * TITLE_BAR_TITLE_GAP_RATIO));

    // Get the font to use and center the text vertically
    ImFont* titleFont = UI::Theme::get().titleFont();
    if (titleFont != nullptr)
    {
        ImGui::PushFont(titleFont);
    }

    // Now get the font size after pushing (ImGui::GetFontSize() returns current font size)
    const float fontSize = ImGui::GetFontSize();
    const float titleY = centerY - (fontSize * 0.5F);
    ImGui::SetCursorPosY(titleY);

    ImGui::TextColored(scheme.textPrimary, "TaskSmack");
    const float wordmarkWidth = ImGui::GetItemRectSize().x;

    if (titleFont != nullptr)
    {
        ImGui::PopFont();
    }

    // Right side buttons
    const float BUTTON_WIDTH = computeTitleBarButtonWidth(titleBarHeight, TITLE_BAR_BUTTON_ASPECT);

    // What the bar needs to show, for the layer's minimum window size (#1207).
    output.contentWidth = computeTitleBarContentWidth(iconX,
                                                      ICON_SIZE,
                                                      titleBarHeight * TITLE_BAR_TITLE_GAP_RATIO,
                                                      wordmarkWidth,
                                                      BUTTON_WIDTH,
                                                      titleBarHeight * TITLE_BAR_SEPARATOR_GAP_RATIO);
    const float BUTTON_HEIGHT = titleBarHeight;
    // Where every button is drawn, and so what isPointInControlArea() keeps out of the drag area.
    output.layout = computeTitleBarButtonLayout(windowWidth, BUTTON_WIDTH, BUTTON_HEIGHT, titleBarHeight * TITLE_BAR_SEPARATOR_GAP_RATIO);
    const ImVec2 buttonSize(BUTTON_WIDTH, BUTTON_HEIGHT);

    // Window control buttons (right to left: Close, Maximize, Minimize)
    // titleBgActive with zero alpha gives a transparent resting state; if ImGui ever composites
    // the RGB channel at alpha=0, we blend against the actual title bar background color.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_Button, UI::withAlpha(scheme.titleBgActive, 0.0F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, scheme.buttonHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, scheme.buttonActive);

    // These controls draw Font Awesome glyphs, which are merged into every body font at that font's
    // size. Left under the globally pushed body font they grew and shrank with the Font Size setting
    // inside their now-fixed boxes, so the chrome was only half independent of it. The dedicated
    // fixed-size icon font keeps them proportional to the bar instead.
    ImFont* chromeIcons = UI::Theme::get().chromeIconFont();
    const bool pushedChromeIcons = chromeIcons != nullptr;
    if (pushedChromeIcons)
    {
        ImGui::PushFont(chromeIcons);
    }

    // Close button (hover/active colors from theme)
    ImGui::SetCursorPos(ImVec2(output.layout.close.minX, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, scheme.closeButtonHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, scheme.closeButtonActive);
    // The X is drawn over an unlabelled button rather than passed as that button's label: fa-xmark
    // fills much less of its em box than the icons beside it, so it needs both its own font size
    // and centring on its ink, neither of which a button label can express. The labelled form is
    // the fallback for when the chrome icon font failed to load.
    const char* closeLabel = (chromeIcons != nullptr) ? "##Close" : ICON_FA_XMARK "##Close";
    if (ImGui::Button(closeLabel, buttonSize))
    {
        output.action = Action::Close;
    }
    drawChromeGlyphMatched(chromeIcons,
                           UI::Theme::get().chromeIconFontSizePx(),
                           ICON_FA_XMARK,
                           CHROME_GLYPH_XMARK,
                           ImGui::GetItemRectMin(),
                           ImGui::GetItemRectMax());
    ImGui::PopStyleColor(2);

    // Maximize/Restore button
    ImGui::SetCursorPos(ImVec2(output.layout.maximize.minX, 0));
    const bool isMaximized = input.maximized;
    if (ImGui::Button(isMaximized ? ICON_FA_WINDOW_RESTORE "##Restore" : ICON_FA_WINDOW_MAXIMIZE "##Maximize", buttonSize))
    {
        output.action = isMaximized ? Action::Restore : Action::Maximize;
    }

    // Minimize button
    ImGui::SetCursorPos(ImVec2(output.layout.minimize.minX, 0));
    if (ImGui::Button(ICON_FA_WINDOW_MINIMIZE "##Minimize", buttonSize))
    {
        output.action = Action::Minimize;
    }

    // The app buttons, after the separator gap the layout leaves: Settings, then Help (the "?",
    // #172), then About (the "i", #1600). Their tooltips are shown after the chrome icon font is
    // popped: that font has no letters (#1200).
    ImGui::SetCursorPos(ImVec2(output.layout.settings.minX, 0));
    if (ImGui::Button(TitleBarButtons::SETTINGS_LABEL, buttonSize))
    {
        output.action = Action::Settings;
    }
    const char* tooltip = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) ? TitleBarButtons::SETTINGS_TOOLTIP : nullptr;

    ImGui::SetCursorPos(ImVec2(output.layout.help.minX, 0));
    if (ImGui::Button(TitleBarButtons::HELP_LABEL, buttonSize))
    {
        output.action = Action::Help;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
    {
        tooltip = TitleBarButtons::HELP_TOOLTIP;
    }

    ImGui::SetCursorPos(ImVec2(output.layout.about.minX, 0));
    if (ImGui::Button(TitleBarButtons::ABOUT_LABEL, buttonSize))
    {
        output.action = Action::About;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
    {
        tooltip = TitleBarButtons::ABOUT_TOOLTIP;
    }

    if (pushedChromeIcons)
    {
        ImGui::PopFont();
    }
    if (tooltip != nullptr)
    {
        ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::PopStyleColor(3); // Button colors
    ImGui::PopStyleVar(2);   // Frame padding, item spacing

    // Alt+Space is handled in onSDLEvent() for reliable capture

    // Handle system menu popup (must be within the window context)
    if (input.openSystemMenu)
    {
        ImGui::OpenPopup(SYSTEM_MENU_ID);
    }

    // Render the system menu popup
    if (const Action chosen = renderSystemMenu(titleBarHeight, input.maximized); chosen != Action::None)
    {
        output.action = chosen;
    }

    ImGui::End();

    if (!input.maximized)
    {
        constexpr float BORDER_THICKNESS = 1.0F;
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        drawList->AddRect(ImVec2(0.0F, 0.0F),
                          ImVec2(windowWidth, windowHeight),
                          ImGui::ColorConvertFloat4ToU32(scheme.border),
                          0.0F,
                          ImDrawFlags_None,
                          BORDER_THICKNESS);
    }

    ImGui::PopStyleColor(); // WindowBg
    ImGui::PopStyleVar(2);  // WindowPadding, WindowBorderSize
    return output;
}

} // namespace App::TitleBarView
