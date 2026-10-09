#include "App/AboutDialog.h"

#include "App/DialogGeometry.h"
#include "App/PlatformOpen.h"
#include "UI/ChromeLayout.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/IconLoader.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"
#include "version.h"

#include <imgui.h>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

namespace App::AboutDialog
{

namespace
{

constexpr const char* OK_LABEL = "OK";
constexpr const char* REPO_URL = "https://github.com/mgradwohl/tasksmack";
constexpr const char* FONT_AWESOME_LICENSE_URL = "https://fontawesome.com/license/free";
// Short forms of the URLs, shown as the links' text so they can wrap in a narrow dialog; the full
// URL is the tooltip.
constexpr std::string_view REPO_LABEL = "github.com/mgradwohl/tasksmack";
constexpr std::string_view FONT_AWESOME_LICENSE_LABEL = "fontawesome.com/license/free";

/// Text wrapped at the right edge of the cell or window it is in.
void wrapped(std::string_view text)
{
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
}

/// A link: @p label in the accent colour, wrapped at the right edge of its cell like the text around
/// it, that opens @p url in the system browser when clicked or activated from the keyboard (#1212).
/// The full URL shows as a tooltip and the cursor becomes a hand; a one-line link is underlined on
/// hover.
///
/// The label is a short form of the URL and wraps: a one-line Selectable as wide as the full URL
/// could not shrink with the dialog, and was clipped by a narrow window at a large font (#1490
/// review). An invisible button over the wrapped text is what makes it clickable and focusable.
void renderLink(std::string_view label, const char* url, const ImVec4& color)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    wrapped(label);
    ImGui::PopStyleColor();
    const ImVec2 textMin = ImGui::GetItemRectMin();
    const ImVec2 textMax = ImGui::GetItemRectMax();

    ImGui::SetCursorScreenPos(textMin);
    ImGui::PushID(url);
    const bool pressed =
        // EnableNav: InvisibleButton opts out of keyboard navigation by default, and a link must be
        // reachable and activatable from the keyboard like every other control (#1542 review).
        ImGui::InvisibleButton(
            "##Link", ImVec2(std::max(1.0F, textMax.x - textMin.x), std::max(1.0F, textMax.y - textMin.y)), ImGuiButtonFlags_EnableNav);
    ImGui::PopID();
    if (pressed)
    {
        (void) App::PlatformOpen::openWithSystemHandler(std::string_view{url});
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", url);
        if ((textMax.y - textMin.y) < (ImGui::GetTextLineHeight() * 1.5F))
        {
            ImGui::GetWindowDrawList()->AddLine(ImVec2(textMin.x, textMax.y), textMax, ImGui::GetColorU32(color));
        }
    }
}

/// Text in the theme's muted colour, wrapped at the right edge of the cell or window it is in.
void mutedWrapped(std::string_view text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::get().scheme().textMuted);
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

/// Draws the icon, @p iconPx on its longest edge, or an empty space as large when there is none.
void renderIcon(const UI::Texture& icon, float iconPx)
{
    if (icon.valid())
    {
        const ImVec2 rawSize = icon.size();
        const float scale = std::min(iconPx / rawSize.x, iconPx / rawSize.y);
        ImGui::Image(icon.textureId(), ImVec2(rawSize.x * scale, rawSize.y * scale));
    }
    else
    {
        ImGui::Dummy(ImVec2(iconPx, iconPx));
    }
}

/// The name in the large font, then the version and the tagline, which wrap.
void renderHeaderText(ImFont* titleFont)
{
    if (titleFont != nullptr)
    {
        ImGui::PushFont(titleFont);
    }
    wrapped("TaskSmack");
    if (titleFont != nullptr)
    {
        ImGui::PopFont();
    }
    const std::string version = std::format("Version {} ({} build)", tasksmack::Version::STRING, tasksmack::Version::BUILD_TYPE);
    mutedWrapped(version);
    // The name is already the large title above, so the tagline does not repeat it (#1212).
    wrapped("A cross-platform system monitor");
}

/// The icon beside the name, version and tagline, the two centred on each other -- or, when the
/// dialog is too narrow for both on one row (a large font in a small window), the icon above the
/// text, so neither is clipped (#1490 review). A two-column table keeps the side-by-side halves on
/// one row without SameLine()/SetCursorPosY() arithmetic fighting ImGui's line tracking.
void renderHeader(const UI::Texture& icon, float emPx)
{
    const auto& theme = UI::Theme::get();
    const ImGuiStyle& style = ImGui::GetStyle();

    ImFont* titleFont = theme.largeFont();
    float titleHeight = ImGui::GetTextLineHeight();
    float titleWidth = ImGui::CalcTextSize("TaskSmack").x;
    if (titleFont != nullptr)
    {
        ImGui::PushFont(titleFont);
        titleHeight = ImGui::GetTextLineHeight();
        titleWidth = ImGui::CalcTextSize("TaskSmack").x;
        ImGui::PopFont();
    }
    const float iconPx = ABOUT_ICON_EM * emPx;
    const float gapPx = ABOUT_HEADER_GAP_EM * emPx;
    const float availPx = ImGui::GetContentRegionAvail().x;

    if (!UI::DialogMetrics::fitsSideBySide(availPx, iconPx, gapPx, std::max(titleWidth, ABOUT_HEADER_MIN_TEXT_EM * emPx)))
    {
        renderIcon(icon, std::min(iconPx, availPx));
        renderHeaderText(titleFont);
        return;
    }

    const float textBlockHeight = titleHeight + (ImGui::GetTextLineHeight() * 2.0F) + (style.ItemSpacing.y * 2.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(gapPx * 0.5F, 0.0F));
    if (ImGui::BeginTable("##AboutHeader", 2, ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("##Icon", ImGuiTableColumnFlags_WidthFixed, iconPx);
        ImGui::TableSetupColumn("##Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        const float rowTop = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(rowTop + std::max(0.0F, (textBlockHeight - iconPx) * 0.5F));
        renderIcon(icon, iconPx);

        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(rowTop + std::max(0.0F, (iconPx - textBlockHeight) * 0.5F));
        renderHeaderText(titleFont);

        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

/// Starts a label / value table: a muted label column fitted to its widest label, then the values.
bool beginDetailsTable(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit))
    {
        return false;
    }
    ImGui::TableSetupColumn("##Label", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##Value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

/// The next row of a beginDetailsTable() table, its label drawn; the cursor is left in the value cell.
void detailsRow(const char* label)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(UI::Theme::get().scheme().textMuted, "%s", label);
    ImGui::TableNextColumn();
}

void renderDetails()
{
    const auto& theme = UI::Theme::get();
    (void) UI::Widgets::sectionHeader(ICON_FA_CIRCLE_INFO, "Project");
    if (beginDetailsTable("##AboutProject"))
    {
        detailsRow("Source");
        renderLink(REPO_LABEL, REPO_URL, theme.accentColor(0));
        detailsRow("License");
        ImGui::TextUnformatted("MIT");
        detailsRow("Commit");
        ImGui::TextUnformatted(tasksmack::Version::GIT_COMMIT);
        ImGui::EndTable();
    }
}

void renderCredits()
{
    const auto& theme = UI::Theme::get();
    // Every bundled font and icon set with its licence (#1212). Font Awesome Free is licensed in two
    // parts: the icon designs under CC BY 4.0 (which requires this attribution) and the distributed
    // fa-solid-900.ttf under the SIL OFL 1.1. Full notices: assets/fonts/LICENSE.txt.
    (void) UI::Widgets::sectionHeader(ICON_FA_FONT, "Credits");
    if (beginDetailsTable("##AboutCredits"))
    {
        detailsRow("Fonts");
        wrapped("Inter, Sixtyfour");
        mutedWrapped("SIL Open Font License 1.1");
        detailsRow("Icons");
        wrapped("Font Awesome Free by Fonticons, Inc.");
        mutedWrapped("Icons CC BY 4.0, font SIL Open Font License 1.1");
        renderLink(FONT_AWESOME_LICENSE_LABEL, FONT_AWESOME_LICENSE_URL, theme.accentColor(0));
        ImGui::EndTable();
    }
}

// The keyboard shortcuts were listed here until Help became a window of its own (#172): About is
// product information only, and F1 opens App::HelpWindow.

} // namespace

void render(bool& openRequested, const UI::Texture& icon)
{
    if (!openRequested && !ImGui::IsPopupOpen(POPUP_ID))
    {
        return; // Nothing to draw when neither open nor requested
    }

    if (openRequested)
    {
        ImGui::OpenPopup(POPUP_ID);
        openRequested = false;
    }

    // Centred on every frame, as Settings is, not only when it appears: its height settles over its
    // first frames (the body fits itself to its contents), and centred once on the first, shorter
    // frame it ended up hanging off the bottom of the window. It is NoMove, so this never fights
    // the user.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5F, 0.5F));

    // A compact dialog of its own width from the first frame, its height fitted to its contents but
    // never more than ABOUT_MAX_HEIGHT_FRACTION of the window (#1490). It used to auto-fit both axes
    // around a shortcut table told to fill the dialog's width: each frame the table's wrapped text
    // asked for a little more than the width it was given, so the dialog crept wider frame by frame
    // -- the slow "grow" -- until it filled most of the window. A width set every frame (as
    // ProcessBatchPriorityDialog does) gives the wrapped text nothing to feed back into. Both caps
    // are re-evaluated every frame, so a font change or a shrinking window can't push the OK button
    // out of reach (#1129).
    const float emPx = ImGui::GetFontSize();
    const ImVec2 dialogMaxSize(UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x),
                               UI::DialogMetrics::computeCompactDialogMaxExtent(viewport->WorkSize.y, ABOUT_MAX_HEIGHT_FRACTION));
    ImGui::SetNextWindowSize(ImVec2(UI::DialogMetrics::computeDialogWidth(emPx, ABOUT_WIDTH_EM, viewport->WorkSize.x), 0.0F));
    UI::Widgets::setNextDialogSizeConstraints(dialogMaxSize);

    const float marginPx = ABOUT_MARGIN_EM * emPx;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(marginPx, marginPx));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;
    const bool open = ImGui::BeginPopupModal(POPUP_ID, nullptr, flags);
    ImGui::PopStyleVar(); // The modal keeps its padding; the body and tables use the app's own
    if (!open)
    {
        return;
    }

    UI::Widgets::keepCurrentWindowInViewport();
    const auto& theme = UI::Theme::get();
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);

    // Escape closes the dialog, as OK does; ImGui's own Escape handling leaves modals open.
    bool close = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    // Everything above the OK row scrolls in a child fitted to its content but never taller than
    // leaves room for that row, so the button stays in the dialog at any font size in any window
    // height (#1129): the body scrolls instead of growing the dialog (#1490). Reserved: the
    // title bar, the window padding, and the footer with the item spacing before it -- laid out here
    // from the inputs dialogFooter() will see, so the height reserved is the height it takes.
    const float okWidth = UI::Widgets::footerButtonWidth({OK_LABEL}, ABOUT_BUTTON_MIN_EM);
    const float footerHeight =
        style.ItemSpacing.y + UI::ChromeLayout::layoutDialogFooter(
                                  UI::Widgets::dialogFooterInput(okWidth, false, nullptr), style.ItemSpacing.y, ImGui::GetFrameHeight())
                                  .height;
    const float reservedHeight = ImGui::GetFrameHeight() + (marginPx * 2.0F) + footerHeight;
    const float bodyMaxHeight =
        UI::DialogMetrics::computeScrollableBodyMaxHeight(dialogMaxSize.y, reservedHeight, ImGui::GetFrameHeightWithSpacing() * 2.0F);
    UI::Widgets::setNextDialogSizeConstraints(ImVec2(dialogMaxSize.x, bodyMaxHeight));
    ImGui::BeginChild("##AboutBody", ImVec2(0.0F, 0.0F), ImGuiChildFlags_AutoResizeY);
    renderHeader(icon, emPx);
    UI::Widgets::sectionGap();
    renderDetails();
    UI::Widgets::sectionGap();
    renderCredits();
    ImGui::EndChild(); // ##AboutBody

    ImGui::PopStyleColor();

    // The footer every dialog shares, OK on the right (#1200).
    const UI::Widgets::DialogFooterButton okButton{.label = OK_LABEL, .fills = nullptr, .tooltip = nullptr};
    if (UI::Widgets::dialogFooter(okButton, {}, okWidth) == UI::Widgets::DialogFooterAction::Primary)
    {
        close = true;
    }
    if (close)
    {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace App::AboutDialog
