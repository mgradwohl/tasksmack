#include "App/HelpWindow.h"

#include "App/DialogGeometry.h"
#include "App/HelpContent.h"
#include "App/KeyboardShortcuts.h"
#include "App/PlatformOpen.h"
#include "App/ProcessColumnConfig.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace App::HelpWindow
{

namespace
{

/// Text wrapped at the right edge of the cell or window it is in.
void wrapped(std::string_view text)
{
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
}

constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX;
constexpr float KEYS_COLUMN_WEIGHT = 2.0F;
constexpr float TEXT_COLUMN_WEIGHT = 3.0F;

/// Starts a two-column table, @p firstWeight to @p secondWeight; both columns wrap.
bool beginTwoColumnTable(const char* id, float firstWeight, float secondWeight)
{
    if (!ImGui::BeginTable(id, 2, TABLE_FLAGS))
    {
        return false;
    }
    ImGui::TableSetupColumn("##First", ImGuiTableColumnFlags_WidthStretch, firstWeight);
    ImGui::TableSetupColumn("##Second", ImGuiTableColumnFlags_WidthStretch, secondWeight);
    return true;
}

void mutedWrapped(std::string_view text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::get().scheme().textMuted);
    wrapped(text);
    ImGui::PopStyleColor();
}

/// The filter box, then one table per area with the shortcuts the filter keeps; an area with none is
/// left out. The filter is matched only when it is edited (HelpContent::matchingShortcuts()).
void renderShortcuts(State& state)
{
    const auto& theme = UI::Theme::get();
    (void) UI::Widgets::sectionHeader(ICON_FA_LIST, "Keyboard shortcuts");

    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::InputTextWithHint(FILTER_ID, "Type to find a shortcut", state.filter.data(), state.filter.size()))
    {
        state.visible = HelpContent::matchingShortcuts(std::string_view{state.filter.data()});
        state.visibleCount = HelpContent::countMatches(state.visible);
    }

    std::size_t rows = 0;
    if (state.visibleCount == 0)
    {
        mutedWrapped("No shortcut matches the filter.");
    }
    for (const KeyboardShortcuts::ShortcutArea area : KeyboardShortcuts::SHORTCUT_AREAS)
    {
        const auto inArea = [area, &state](std::size_t i)
        {
            return state.visible.at(i) && KeyboardShortcuts::SHORTCUT_HELP.at(i).area == area;
        };
        bool any = false;
        for (std::size_t i = 0; i < KeyboardShortcuts::SHORTCUT_HELP.size() && !any; ++i)
        {
            any = inArea(i);
        }
        if (!any)
        {
            continue;
        }
        const std::string_view label = KeyboardShortcuts::areaLabel(area);
        ImGui::Spacing();
        mutedWrapped(label);
        ImGui::PushID(static_cast<int>(area));
        if (beginTwoColumnTable("##Shortcuts", KEYS_COLUMN_WEIGHT, TEXT_COLUMN_WEIGHT))
        {
            for (std::size_t i = 0; i < KeyboardShortcuts::SHORTCUT_HELP.size(); ++i)
            {
                if (!inArea(i))
                {
                    continue;
                }
                const KeyboardShortcuts::ShortcutHelpEntry& entry = KeyboardShortcuts::SHORTCUT_HELP.at(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, theme.accentColor(0));
                wrapped(entry.keys);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                wrapped(entry.description);
                ++rows;
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
    state.shortcutRowsDrawn = rows;
}

void renderTabs()
{
    const auto& theme = UI::Theme::get();
    (void) UI::Widgets::sectionHeader(ICON_FA_CLONE, "Tabs");
    if (beginTwoColumnTable("##Tabs", KEYS_COLUMN_WEIGHT, TEXT_COLUMN_WEIGHT))
    {
        for (const HelpContent::TabSummary& tab : HelpContent::TAB_OVERVIEW)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.accentColor(0));
            wrapped(tab.name);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            wrapped(tab.summary);
        }
        ImGui::EndTable();
    }
}

/// Every Processes column, from the same metadata as the table's header and its tooltip
/// (getColumnInfo(), columnCapabilityNote()), so the reference can't drift from the table.
void renderColumns()
{
    const auto& theme = UI::Theme::get();
    (void) UI::Widgets::sectionHeader(ICON_FA_TABLE_COLUMNS, "Processes columns");
    mutedWrapped("The Processes tab's Columns button shows or hides each one; hover a column's header for the same text.");
    if (beginTwoColumnTable("##Columns", KEYS_COLUMN_WEIGHT, TEXT_COLUMN_WEIGHT))
    {
        for (const ProcessColumn col : allProcessColumns())
        {
            const ProcessColumnInfo info = getColumnInfo(col);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.accentColor(0));
            wrapped(info.menuName);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            wrapped(info.description);
            if (const std::string_view platform = HelpContent::columnNotePlatform(col); !platform.empty())
            {
                mutedWrapped(platform);
                ImGui::SameLine();
                mutedWrapped(columnCapabilityNote(col, /*hasUdpNetworkCounters=*/false));
            }
        }
        ImGui::EndTable();
    }
}

/// A one-line link that opens @p url with the system handler, as About's links do; the URL is its
/// tooltip. Its colour is the caller's ImGuiCol_TextLink.
void link(const char* label, const char* url)
{
    if (ImGui::TextLink(label))
    {
        (void) PlatformOpen::openWithSystemHandler(std::string_view{url});
    }
    ImGui::SetItemTooltip("%s", url);
}

void renderLinks()
{
    (void) UI::Widgets::sectionHeader(ICON_FA_CIRCLE_QUESTION, "More help");
    link("User guide (online)", HelpContent::USER_GUIDE_URL);
    link("Report a problem or ask for a feature", HelpContent::ISSUES_URL);
}

} // namespace

void requestOpen(State& state) noexcept
{
    state.open = true;
    state.focusRequested = true;
}

Action render(State& state)
{
    if (!state.open)
    {
        return Action::None;
    }

    // First opened centred at its authored size; from then on where and as large as the user left it,
    // for the session (ImGui keeps the window; NoSavedSettings keeps it out of any ini). The size cap
    // follows the window every frame, and keepCurrentWindowInViewport() pulls it back if the main
    // window shrinks under it (#1129).
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float emPx = ImGui::GetFontSize();
    const ImVec2 maxSize(UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x),
                         UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.y));
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(UI::DialogMetrics::computeDialogWidth(emPx, HELP_WIDTH_EM, viewport->WorkSize.x),
                                    std::min(maxSize.y, viewport->WorkSize.y * HELP_HEIGHT_FRACTION)),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(std::min(HELP_MIN_WIDTH_EM * emPx, maxSize.x), std::min(HELP_MIN_HEIGHT_EM * emPx, maxSize.y)), maxSize);
    if (state.focusRequested)
    {
        ImGui::SetNextWindowFocus();
        state.focusRequested = false;
    }

    Action action = Action::None;
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin(WINDOW_ID, &state.open, flags))
    {
        UI::Widgets::keepCurrentWindowInViewport();
        const auto& theme = UI::Theme::get();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);
        ImGui::PushStyleColor(ImGuiCol_TextLink, theme.accentColor(0)); // Links in the accent colour, as About's are

        // Escape closes it while it has focus, unless a text field (the filter) has the keyboard:
        // there Escape belongs to the field.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            state.open = false;
        }

        // The body scrolls; the footer below it stays in view.
        const float footerHeight = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild(BODY_ID, ImVec2(0.0F, -footerHeight)))
        {
            renderShortcuts(state);
            UI::Widgets::sectionGap();
            renderTabs();
            UI::Widgets::sectionGap();
            renderColumns();
            UI::Widgets::sectionGap();
            renderLinks();
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (ImGui::TextLink(ABOUT_LINK_LABEL))
        {
            action = Action::OpenAbout;
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::End();
    return action;
}

} // namespace App::HelpWindow
