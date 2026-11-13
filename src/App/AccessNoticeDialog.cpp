#include "AccessNoticeDialog.h"

#include "App/AccessChoice.h"
#include "App/AccessTable.h"
#include "App/Panels/AuthorizeWording.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/FrameOutline.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace App::AccessNoticeDialog
{

namespace
{

/// The notice's authored width in ems: room for the access table's three columns.
constexpr float WIDTH_EM = 46.0F;
/// Its footer buttons' floor, in ems.
constexpr float BUTTON_MIN_EM = 9.375F;

void wrappedText(std::string_view text)
{
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
}

/// The access table (AccessTable.h): what a Standard session gets, and what an Administrator one adds.
void renderAccessTable(AccessChoice::AccessPlatform platform)
{
    constexpr ImGuiTableFlags FLAGS = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##AccessTable", 3, FLAGS))
    {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 2.0F);
        ImGui::TableSetupColumn("Standard", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("Administrator", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableHeadersRow();
        for (const AccessTable::Row& row : AccessTable::rowsFor(platform))
        {
            ImGui::TableNextRow();
            for (const std::string_view cell : {row.item, row.standard, row.administrator})
            {
                ImGui::TableNextColumn();
                wrappedText(cell);
            }
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    wrappedText(AccessTable::neverRowFor(platform));
}

/// Windows: what Administrator does, what the prompt asks for, and how the last restart went.
void renderRestartText(const State& state)
{
    const auto& scheme = UI::Theme::get().scheme();
    wrappedText(RESTART_ADMINISTRATOR_TEXT);
    ImGui::Spacing();
    if (state.restart == Restart::Unavailable)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
        wrappedText(RESTART_UNAVAILABLE);
        ImGui::PopStyleColor();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
    const std::string_view promptNote = state.credentials ? CREDENTIALS_NOTE : CONSENT_NOTE;
    wrappedText(state.restart == Restart::Waiting ? WAITING_NOTE : promptNote);
    ImGui::PopStyleColor();
    if (!state.outcome.empty())
    {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textWarning);
        wrappedText(state.outcome);
        ImGui::PopStyleColor();
    }
}

} // namespace

void requestOpen(State& state)
{
    state.openRequested = true;
    state.dontShowAgain = false;
    state.outcome.clear();
}

float computeWidth(float viewportWidthPx)
{
    return std::floor(std::min(WIDTH_EM * ImGui::GetFontSize(), UI::DialogMetrics::computeDialogMaxExtent(viewportWidthPx)));
}

Action render(State& state)
{
    if (!state.openRequested && !ImGui::IsPopupOpen(POPUP_ID))
    {
        return Action::None;
    }
    if (state.openRequested)
    {
        ImGui::OpenPopup(POPUP_ID);
        state.openRequested = false;
        state.centredViewportWidth = 0.0F; // Centre it afresh below
    }

    // Its width every frame (auto-resize would otherwise take over from the second frame), centred
    // when it opens and again when the window or its own height changes, as the other notices are.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(computeWidth(viewport->WorkSize.x), 0.0F));
    constexpr float RECENTRE_THRESHOLD_PX = 0.5F;
    const auto moved = [](float now, float centredFor)
    {
        return std::abs(now - centredFor) > RECENTRE_THRESHOLD_PX;
    };
    if (moved(viewport->WorkSize.x, state.centredViewportWidth) || moved(viewport->WorkSize.y, state.centredViewportHeight) ||
        moved(state.dialogHeight, state.centredDialogHeight))
    {
        ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
        state.centredViewportWidth = viewport->WorkSize.x;
        state.centredViewportHeight = viewport->WorkSize.y;
        state.centredDialogHeight = state.dialogHeight;
    }
    UI::Widgets::setNextDialogSizeConstraints(ImVec2(UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x),
                                                     UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.y)));

    Action action = Action::None;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;
    if (ImGui::BeginPopupModal(POPUP_ID, nullptr, flags))
    {
        UI::Widgets::keepCurrentWindowInViewport();
        state.dialogHeight = ImGui::GetWindowHeight();
        const auto& scheme = UI::Theme::get().scheme();

        ImGui::TextColored(scheme.textWarning, ICON_FA_LOCK "  Some data is limited");
        ImGui::Separator();
        ImGui::Spacing();
        wrappedText(STANDARD_TEXT);
        ImGui::Spacing();
        const bool windows = state.platform == AccessChoice::AccessPlatform::Windows;
        if (windows)
        {
            renderRestartText(state);
        }
        else
        {
            wrappedText(START_ADMINISTRATOR_TEXT);
        }

        UI::Widgets::sectionGap();
        if (ImGui::CollapsingHeader(TABLE_HEADER))
        {
            renderAccessTable(state.platform);
        }

        UI::Widgets::sectionGap();
        UI::Widgets::outlinedCheckbox("Don't show this again", &state.dontShowAgain);
        ImGui::SetItemTooltip("Stops this note at startup. The Standard chip in the status bar shows it again, and Settings > "
                              "Advanced > Show limited-data notice turns it back on");

        UI::Widgets::DialogFooterAction pressed = UI::Widgets::DialogFooterAction::None;
        if (windows)
        {
            // Continue as Standard is the default, rightmost; Restart beside it, disabled while Windows asks.
            const UI::Widgets::DialogFooterButton standard{.label = CONTINUE_LABEL, .fills = nullptr, .tooltip = nullptr};
            const UI::Widgets::DialogFooterButton restart{
                .label = RESTART_LABEL,
                .fills = nullptr,
                .tooltip = state.restart == Restart::Unavailable ? RESTART_UNAVAILABLE : AuthorizeWording::WINDOWS_WORDING.promptTooltip,
                .disabled = state.restart != Restart::Ready,
            };
            pressed = UI::Widgets::dialogFooter(
                standard, restart, UI::Widgets::footerButtonWidth({CONTINUE_LABEL, RESTART_LABEL}, BUTTON_MIN_EM));
        }
        else
        {
            const UI::Widgets::DialogFooterButton ok{.label = OK_LABEL, .fills = nullptr, .tooltip = nullptr};
            pressed = UI::Widgets::dialogFooter(ok, {}, UI::Widgets::footerButtonWidth({OK_LABEL}, BUTTON_MIN_EM));
        }
        if (pressed == UI::Widgets::DialogFooterAction::Secondary)
        {
            action = Action::RestartAsAdministrator; // stays up while Windows asks
        }
        else if (pressed == UI::Widgets::DialogFooterAction::Primary || UI::Widgets::dialogEscapePressed())
        {
            action = Action::Closed;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return action;
}

} // namespace App::AccessNoticeDialog
