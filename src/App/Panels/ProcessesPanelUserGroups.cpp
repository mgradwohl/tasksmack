// The Processes table's Group: User, with the Users tab folded into it (#3377): each signed-in account's
// sessions under its header, their status and sign-in time, and their Lock, Disconnect and Sign out. The
// groups are built in UserGrouping (buildUserGroups()), from the sessions UserSessionsFeed publishes; this is
// the panel's drawing of them, apart from ProcessesPanel.cpp, which draws every other row.

#include "App/Panels/ProcessGrouping.h"
#include "App/Panels/ProcessTreeIndent.h"
#include "App/Panels/ProcessesPanel.h"
#include "App/Panels/SessionActionControl.h"
#include "App/Panels/SessionActionView.h"
#include "App/Panels/UserGrouping.h"
#include "App/Panels/UserSessionsFeed.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/UsersModel.h"
#include "Platform/IUserSessions.h"
#include "UI/CellText.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace App
{

namespace
{

constexpr const char* SESSION_MENU_POPUP_ID = "##UserSessionMenu";

/// Now, as the sessions' idle times are told from (CLOCK_REALTIME µs).
[[nodiscard]] std::uint64_t nowUsec()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

/// Whether the table shows @p column this frame: a hidden one is drawn into nowhere.
[[nodiscard]] bool tableShows(ProcessColumn column)
{
    return (ImGui::TableGetColumnFlags(static_cast<int>(toIndex(column))) & ImGuiTableColumnFlags_IsEnabled) != 0;
}

} // namespace

void ProcessesPanel::refreshSessions()
{
    if (m_UserSessions == nullptr)
    {
        return;
    }
    // Read only while this table shows them, grouped by User (ProcessGrouping::sessionsDemand()); only a
    // change does anything.
    m_UserSessions->setProcessesView(m_IsActiveTab, m_GroupBy);
    const Domain::UsersModel* const model = m_UserSessions->model();
    if (model == nullptr || m_GroupBy != ProcessGrouping::GroupBy::User)
    {
        m_SessionsPublication.reset();
        return;
    }
    if (!m_SessionsPublication || m_SessionsPublication->version != model->version())
    {
        m_SessionsPublication = model->publication(); // shared, not copied: one atomic load with nothing new
    }
    // The session actions' worker: a finished one's line, which goes after a while, and the session's new
    // state read at once.
    if (SessionActionControl* const actions = m_UserSessions->actions(); actions != nullptr)
    {
        actions->update(ImGui::GetIO().DeltaTime);
        if (actions->takeDone())
        {
            m_UserSessions->requestRefresh();
        }
    }
}

void ProcessesPanel::renderSessionsReadFailure() const
{
    // One muted line, and the processes as ever; Unsupported (a synthetic run, no logind) says nothing.
    if (m_GroupBy != ProcessGrouping::GroupBy::User || !m_SessionsPublication ||
        m_SessionsPublication->status != Platform::UserSessionsStatus::Failed)
    {
        return;
    }
    ImGui::TextColored(UI::Theme::get().scheme().textMuted,
                       ICON_FA_TRIANGLE_EXCLAMATION "  Signed-in sessions couldn't be read: %s",
                       m_SessionsPublication->detail.c_str());
}

void ProcessesPanel::renderSessionMenu()
{
    // One popup at panel level, as the row menu is, opened by ID from a session row or a header.
    m_SessionMenuPopupId = ImGui::GetID(SESSION_MENU_POPUP_ID);
    if (!ImGui::BeginPopup(SESSION_MENU_POPUP_ID))
    {
        m_SessionMenuTarget.reset();
        return;
    }
    if (m_SessionMenuTarget.has_value() && m_UserSessions != nullptr)
    {
        // The Users tab's items, for the copy taken at the right-click (#1365): a regroup can't change it.
        const SessionMenuChoice choice = renderSessionMenuItems(m_UserSessions->sessionMenu(), *m_SessionMenuTarget);
        if (SessionActionControl* const actions = m_UserSessions->actions(); actions != nullptr && choice.any())
        {
            startSessionChoice(*actions, choice);
        }
    }
    ImGui::EndPopup();
}

void ProcessesPanel::renderSessionRow(const Platform::UserSession& session)
{
    ImGui::TableNextRow();
    const auto& scheme = UI::Theme::get().scheme();
    const std::string status = UserGrouping::sessionStatus(session, nowUsec());
    const bool statusColumnShown = tableShows(ProcessColumn::Status);
    int colIdx = 0;
    for (const ProcessColumn col : allProcessColumns())
    {
        const bool columnVisible = ImGui::TableSetColumnIndex(colIdx);
        ++colIdx;
        if (col == ProcessColumn::PID)
        {
            // The row's hover and right-click target, as a process row's; a click selects nothing.
            ImGui::PushID("session");
            ImGui::PushID(session.id.c_str());
            static_cast<void>(
                ImGui::Selectable("##session", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) // built only when shown, not every frame
            {
                ImGui::SetTooltip("%s", UserGrouping::sessionTooltip(session).c_str());
            }
            // Nothing is offered on a session not readable: who it belongs to can't be confirmed.
            if (m_UserSessions != nullptr && session.unreadable.empty() && m_UserSessions->sessionMenu().any() &&
                ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                m_SessionMenuTarget = session;
                ImGui::OpenPopup(m_SessionMenuPopupId);
            }
            ImGui::PopID();
            ImGui::PopID();
            continue;
        }
        if (!columnVisible)
        {
            continue;
        }
        if (col == ProcessColumn::Name)
        {
            // One level in, as the account's processes are; the status beside the place when Status is hidden.
            const float indent = ProcessTreeIndent::INDENT_PER_LEVEL_EM * ImGui::GetFontSize();
            ImGui::Indent(indent);
            std::string name = std::string(ICON_FA_DESKTOP "  ") + UserGrouping::sessionRowName(session);
            if (!statusColumnShown)
            {
                name += " \xC2\xB7 " + status; // " · "
            }
            ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
            UI::Widgets::cellText(name);
            ImGui::PopStyleColor();
            ImGui::Unindent(indent);
        }
        else if (col == ProcessColumn::Status)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
            UI::Widgets::cellText(status);
            ImGui::PopStyleColor();
        }
        else if (col == ProcessColumn::StartTime)
        {
            if (const std::string signedIn = UserGrouping::sessionSignInTime(session); !signedIn.empty())
            {
                UI::Widgets::cellText(signedIn,
                                      {.rightAligned = true, .clippedTooltip = true, .layoutFullText = true, .trailingReservePx = 0.0F});
            }
        }
    }
}

void ProcessesPanel::renderNoMatchesRow()
{
    ImGui::TableNextRow();
    if (!ImGui::TableSetColumnIndex(static_cast<int>(toIndex(ProcessColumn::Name))))
    {
        return;
    }
    const float indent = ProcessTreeIndent::INDENT_PER_LEVEL_EM * ImGui::GetFontSize();
    ImGui::Indent(indent);
    ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::get().scheme().textMuted);
    UI::Widgets::cellText(UserGrouping::NO_MATCHING_PROCESSES);
    ImGui::PopStyleColor();
    ImGui::Unindent(indent);
}

} // namespace App
