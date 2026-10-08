#include "App/AboutDialog.h"

#include "App/DialogGeometry.h"
#include "App/KeyboardShortcuts.h"
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
#include <string_view>

namespace App::AboutDialog
{

namespace
{

constexpr const char* OK_LABEL = "OK";
constexpr const char* REPO_URL = "https://github.com/mgradwohl/tasksmack";
constexpr const char* FONT_AWESOME_LICENSE_URL = "https://fontawesome.com/license/free";

/// A URL drawn in the accent colour, underlined on hover with a hand cursor, that opens in the
/// system browser when clicked or activated from the keyboard (#1212). Sized to its text: a
/// Selectable left at its default width highlighted the whole row on hover, which read as a list
/// item rather than a link (#1490).
void renderLink(const char* url, const ImVec4& color)
{
    const ImVec2 textSize = ImGui::CalcTextSize(url);
    const ImVec2 textPos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
    if (ImGui::Selectable(url, false, ImGuiSelectableFlags_DontClosePopups, textSize))
    {
        (void) App::PlatformOpen::openWithSystemHandler(std::string_view{url});
    }
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const float underlineY = textPos.y + textSize.y;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(textPos.x, underlineY), ImVec2(textPos.x + textSize.x, underlineY), ImGui::GetColorU32(color));
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

/// Text wrapped at the right edge of the cell or window it is in.
void wrapped(std::string_view text)
{
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
}

/// The icon beside the name, version and tagline, the three lines centred on the icon (or the icon
/// on them, whichever is taller). A two-column table keeps the two halves on one row without
/// SameLine()/SetCursorPosY() arithmetic fighting ImGui's line tracking.
void renderHeader(const UI::Texture& icon, float emPx)
{
    const auto& theme = UI::Theme::get();
    const ImGuiStyle& style = ImGui::GetStyle();

    ImFont* titleFont = theme.largeFont();
    float titleHeight = ImGui::GetTextLineHeight();
    if (titleFont != nullptr)
    {
        ImGui::PushFont(titleFont);
        titleHeight = ImGui::GetTextLineHeight();
        ImGui::PopFont();
    }
    const float textBlockHeight = titleHeight + (ImGui::GetTextLineHeight() * 2.0F) + (style.ItemSpacing.y * 2.0F);
    const float iconPx = ABOUT_ICON_EM * emPx;

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ABOUT_HEADER_GAP_EM * emPx * 0.5F, 0.0F));
    if (ImGui::BeginTable("##AboutHeader", 2, ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("##Icon", ImGuiTableColumnFlags_WidthFixed, iconPx);
        ImGui::TableSetupColumn("##Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        const float rowTop = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(rowTop + std::max(0.0F, (textBlockHeight - iconPx) * 0.5F));
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

        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(rowTop + std::max(0.0F, (iconPx - textBlockHeight) * 0.5F));
        if (titleFont != nullptr)
        {
            ImGui::PushFont(titleFont);
        }
        ImGui::TextUnformatted("TaskSmack");
        if (titleFont != nullptr)
        {
            ImGui::PopFont();
        }
        ImGui::TextColored(theme.scheme().textMuted, "Version %s (%s build)", tasksmack::Version::STRING, tasksmack::Version::BUILD_TYPE);
        // The name is already the large title above, so the tagline does not repeat it (#1212).
        wrapped("A cross-platform system monitor");

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
        renderLink(REPO_URL, theme.accentColor(0));
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
        renderLink(FONT_AWESOME_LICENSE_URL, theme.accentColor(0));
        ImGui::EndTable();
    }
}

/// TaskSmack has no separate help page: F1 opens this dialog, which lists every shortcut (#170). Both
/// columns wrap, so at a large font or in a narrow window no shortcut is clipped.
void renderShortcuts()
{
    const auto& theme = UI::Theme::get();
    (void) UI::Widgets::sectionHeader(ICON_FA_LIST, "Keyboard shortcuts");
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable("##Shortcuts", 2, flags))
    {
        constexpr float KEYS_COLUMN_WEIGHT = 2.0F;
        constexpr float DESCRIPTION_COLUMN_WEIGHT = 3.0F;
        ImGui::TableSetupColumn("##Keys", ImGuiTableColumnFlags_WidthStretch, KEYS_COLUMN_WEIGHT);
        ImGui::TableSetupColumn("##Description", ImGuiTableColumnFlags_WidthStretch, DESCRIPTION_COLUMN_WEIGHT);
        for (const KeyboardShortcuts::ShortcutHelpEntry& entry : KeyboardShortcuts::SHORTCUT_HELP)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.accentColor(0));
            wrapped(entry.keys);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            wrapped(entry.description);
        }
        ImGui::EndTable();
    }
}

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
    // height (#1129): the shortcut list scrolls instead of growing the dialog (#1490). Reserved: the
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
    UI::Widgets::sectionGap();
    renderShortcuts();
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
