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
#include <string>
#include <string_view>

namespace App::AccessNoticeDialog
{

namespace
{

/// The notice's authored width in ems: room for the access table's three columns.
constexpr float WIDTH_EM = 46.0F;
/// Its footer buttons' floor, in ems.
constexpr float BUTTON_MIN_EM = 9.375F;

/// What each choice means, a line each; Administrator's in @p platform's terms.
[[nodiscard]] std::string_view standardLine() noexcept
{
    return "Standard: TaskSmack runs as you, as it started.";
}

[[nodiscard]] std::string_view administratorLine(AccessChoice::AccessPlatform platform) noexcept
{
    return platform == AccessChoice::AccessPlatform::Windows
             ? "Administrator: TaskSmack restarts with administrator rights, once Windows approves it."
             : "Administrator: the window stays yours, and after one administrator password TaskSmack's helper reads and "
               "acts for it until TaskSmack closes.";
}

void wrappedText(std::string_view text)
{
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopTextWrapPos();
}

/// The access table's rows a session gets today: a Linux row still waiting on the helper's coverage
/// isn't promised (AccessTable.h).
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
            if (row.needsHelperCoverage)
            {
                continue;
            }
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

} // namespace

void requestOpen(State& state, const AccessChoice::NoticeDecision& decision)
{
    state.openRequested = true;
    state.decision = decision;
    state.dontShowAgain = false;
    state.authenticating = false;
    state.closeRequested = false;
    state.note.clear();
}

void finishAdministrator(State& state, bool succeeded, std::string_view why)
{
    state.authenticating = false;
    if (succeeded)
    {
        state.closeRequested = true;
        state.note.clear();
        return;
    }
    state.note = std::string(DECLINED_TEXT).append(why);
}

float computeWidth(float viewportWidthPx)
{
    return std::floor(std::min(WIDTH_EM * ImGui::GetFontSize(), UI::DialogMetrics::computeDialogMaxExtent(viewportWidthPx)));
}

Choice render(State& state)
{
    if (!state.openRequested && !ImGui::IsPopupOpen(POPUP_ID))
    {
        return Choice::None;
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

    Choice choice = Choice::None;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;
    if (ImGui::BeginPopupModal(POPUP_ID, nullptr, flags))
    {
        UI::Widgets::keepCurrentWindowInViewport();
        state.dialogHeight = ImGui::GetWindowHeight();
        const auto& scheme = UI::Theme::get().scheme();
        const AuthorizeWording::PlatformWording& words = AuthorizeWording::wordingFor(state.platform);

        ImGui::TextColored(scheme.textWarning, ICON_FA_LOCK "  TaskSmack is running as Standard");
        ImGui::Separator();
        ImGui::Spacing();
        wrappedText(standardLine());
        ImGui::Spacing();
        wrappedText(administratorLine(state.platform));
        ImGui::TextDisabled("%.*s", static_cast<int>(std::string_view(words.promptTooltip).size()), words.promptTooltip);
        if (!state.decision.administrator.note.empty())
        {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, scheme.textWarning);
            wrappedText(state.decision.administrator.note);
            ImGui::PopStyleColor();
        }

        UI::Widgets::sectionGap();
        if (ImGui::CollapsingHeader(TABLE_HEADER))
        {
            renderAccessTable(state.platform);
        }

        if (state.authenticating || !state.note.empty())
        {
            UI::Widgets::sectionGap();
            wrappedText(state.authenticating ? WAITING_TEXT : std::string_view(state.note));
        }

        UI::Widgets::sectionGap();
        UI::Widgets::outlinedCheckbox("Don't show this again", &state.dontShowAgain);
        ImGui::SetItemTooltip(
            "Hides this notice; TaskSmack still starts as Standard. Settings > Advanced > Show limited-data notice turns it back on");

        const UI::Widgets::DialogFooterButton standard{.label = CONTINUE_LABEL, .fills = nullptr, .tooltip = nullptr};
        const bool administratorOffered = state.decision.administrator.enabled && !state.authenticating;
        const UI::Widgets::DialogFooterButton administrator{
            .label = words.switchSession,
            .fills = nullptr,
            // The note is one of AccessChoice's string literals, so its data() is terminated.
            .tooltip = state.decision.administrator.enabled ? words.promptTooltip : state.decision.administrator.note.data(),
            .disabled = !administratorOffered};
        const UI::Widgets::DialogFooterAction pressed = UI::Widgets::dialogFooter(
            standard, administrator, UI::Widgets::footerButtonWidth({CONTINUE_LABEL, words.switchSession}, BUTTON_MIN_EM));
        // Escape is Continue as Standard, the default; while the prompt is up neither closes it.
        const bool escaped = UI::Widgets::dialogEscapePressed();
        if (state.closeRequested || (!state.authenticating && (pressed == UI::Widgets::DialogFooterAction::Primary || escaped)))
        {
            choice = state.closeRequested ? Choice::None : Choice::Standard;
            state.closeRequested = false;
            ImGui::CloseCurrentPopup();
        }
        else if (pressed == UI::Widgets::DialogFooterAction::Secondary)
        {
            state.authenticating = true;
            state.note.clear();
            choice = Choice::Administrator;
        }
        ImGui::EndPopup();
    }
    return choice;
}

} // namespace App::AccessNoticeDialog
