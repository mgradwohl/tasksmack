#include "UsersView.h"

#include "App/Panels/ProcessGrouping.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/UsersModel.h"
#include "Platform/AsciiCase.h"
#include "Platform/IUserSessions.h"
#include "UI/CellText.h"
#include "UI/EmptyState.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace App
{

namespace
{

constexpr const char* SESSION_MENU_ID = "##UserSessionMenu";
constexpr std::uint64_t USEC_PER_MINUTE = 60'000'000;

/// "Wayland", "X11", "Console" for logind's session types; the type itself for any other.
[[nodiscard]] std::string typeName(const std::string& type)
{
    if (type == "wayland")
    {
        return "Wayland";
    }
    if (type == "x11")
    {
        return "X11";
    }
    if (type == "tty")
    {
        return "Console";
    }
    if (type == "mir")
    {
        return "Mir";
    }
    return type == "unspecified" ? std::string{} : type;
}

} // namespace

bool isSignInSession(const Platform::UserSession& session)
{
    return session.sessionClass.empty() || session.sessionClass == "user";
}

std::vector<UserRow> buildUserRows(const std::vector<Platform::UserSession>& sessions,
                                   const std::vector<Domain::ProcessSnapshot>& processes)
{
    std::vector<UserRow> rows;
    std::map<std::string, std::size_t> byUser; // user name -> row
    for (const Platform::UserSession& session : sessions)
    {
        if (!isSignInSession(session))
        {
            continue;
        }
        auto [found, added] = byUser.try_emplace(session.user, rows.size());
        if (added)
        {
            rows.push_back(
                UserRow{.user = session.user, .uid = session.uid, .sessions = {}, .totals = {}, .processes = {}, .current = false});
        }
        UserRow& row = rows[found->second];
        row.current = row.current || session.current;
        row.sessions.push_back(session);
    }
    for (std::size_t i = 0; i < processes.size(); ++i)
    {
        if (const auto found = byUser.find(processes[i].user); found != byUser.end())
        {
            UserRow& row = rows[found->second];
            row.totals.add(processes[i]);
            row.processes.push_back(i);
        }
    }
    for (UserRow& row : rows)
    {
        std::ranges::stable_sort(row.processes,
                                 [&processes](std::size_t a, std::size_t b) { return processes[a].cpuPercent > processes[b].cpuPercent; });
    }
    std::ranges::stable_sort(rows, [](const UserRow& a, const UserRow& b) { return Platform::compareIgnoreCase(a.user, b.user) < 0; });
    return rows;
}

std::string sessionPlace(const Platform::UserSession& session)
{
    std::string place;
    const auto add = [&place](const std::string& part)
    {
        if (part.empty())
        {
            return;
        }
        if (!place.empty())
        {
            place += " \xC2\xB7 "; // " · "
        }
        place += part;
    };
    add(session.seat);
    add(session.tty);
    add(typeName(session.type));
    if (session.remote)
    {
        add(session.remoteHost.empty() ? std::string("remote") : "from " + session.remoteHost);
    }
    return place.empty() ? std::string("Session ") + session.id : place;
}

std::string sessionStatus(const Platform::UserSession& session, std::uint64_t nowUsec)
{
    if (session.state == "closing")
    {
        return "Closing";
    }
    if (session.idle)
    {
        if (session.idleSinceUsec == 0 || nowUsec <= session.idleSinceUsec)
        {
            return "Idle";
        }
        const std::uint64_t minutes = (nowUsec - session.idleSinceUsec) / USEC_PER_MINUTE;
        if (minutes == 0)
        {
            return "Idle";
        }
        if (minutes < 60)
        {
            return std::format("Idle for {} min", minutes);
        }
        return std::format("Idle for {} h {} min", minutes / 60, minutes % 60);
    }
    if (session.state == "active")
    {
        return "Active";
    }
    return "Signed in"; // "online": signed in, not the one in front
}

void refreshUserRows(const Domain::UsersPublication& publication,
                     const std::shared_ptr<const std::vector<Domain::ProcessSnapshot>>& processes,
                     std::uint64_t processesVersion,
                     UsersViewState& state)
{
    if (state.builtSessionsVersion == publication.version && state.builtProcessesVersion == processesVersion)
    {
        return;
    }
    state.processes = processes;
    static const std::vector<Domain::ProcessSnapshot> NONE;
    state.rows = buildUserRows(publication.sessions, processes != nullptr ? *processes : NONE);
    state.builtSessionsVersion = publication.version;
    state.builtProcessesVersion = processesVersion;
}

namespace
{

/// The totals columns of a user row: processes, CPU, memory, disk, network.
void totalsCells(const ProcessGrouping::Totals& totals)
{
    ImGui::TableNextColumn();
    UI::Widgets::cellText(UI::Format::formatIntLocalized(totals.processes));
    ImGui::TableNextColumn();
    UI::Widgets::cellText(UI::Format::formatFixedLocalized(totals.cpuPercent, 1, "%"));
    ImGui::TableNextColumn();
    UI::Widgets::cellText(UI::Format::formatBytes(static_cast<double>(totals.resident)));
    ImGui::TableNextColumn();
    UI::Widgets::cellText(UI::Format::formatBytesPerSec(totals.ioRead + totals.ioWrite));
    ImGui::TableNextColumn();
    UI::Widgets::cellText(UI::Format::formatBytesPerSec(totals.netSent + totals.netReceived));
}

/// A user's sessions and processes, under their expanded row.
[[nodiscard]] std::optional<UsersViewAction>
renderUserChildren(const UserRow& row, UsersViewState& state, std::optional<ImGuiID> menuId, std::uint64_t nowUsec)
{
    std::optional<UsersViewAction> action;
    const auto& scheme = UI::Theme::get().scheme();
    for (const Platform::UserSession& session : row.sessions)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(session.id.c_str());
        ImGui::Indent();
        const std::string label = std::string(ICON_FA_DESKTOP "  ") + sessionPlace(session) + (session.current ? "  (this session)" : "");
        ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
        if (menuId.has_value() && ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            state.menuSession = session;
            ImGui::OpenPopup(*menuId);
        }
        ImGui::Unindent();
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::TextColored(scheme.textMuted, "%s", sessionStatus(session, nowUsec).c_str());
        for (int column = 0; column < 5; ++column)
        {
            ImGui::TableNextColumn();
        }
    }
    if (state.processes == nullptr)
    {
        return action;
    }
    for (const std::size_t index : row.processes)
    {
        const Domain::ProcessSnapshot& process = (*state.processes)[index];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(process.pid));
        ImGui::Indent();
        const std::string label = std::format("{}  ({})", process.name, process.pid);
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
        {
            action = UsersViewAction{.selectPid = process.pid,
                                     .selectStartTimeTicks = process.startTimeTicks,
                                     .showDetails = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left),
                                     .lock = std::nullopt,
                                     .signOut = std::nullopt};
        }
        ImGui::Unindent();
        ImGui::PopID();
        ImGui::TableNextColumn(); // Status: none for a process
        ImGui::TableNextColumn(); // Processes: none
        ImGui::TableNextColumn();
        UI::Widgets::cellText(UI::Format::formatFixedLocalized(process.cpuPercent, 1, "%"));
        ImGui::TableNextColumn();
        UI::Widgets::cellText(UI::Format::formatBytes(static_cast<double>(process.memoryBytes)));
        ImGui::TableNextColumn();
        UI::Widgets::cellText(UI::Format::formatBytesPerSec(process.ioReadBytesPerSec + process.ioWriteBytesPerSec));
        ImGui::TableNextColumn();
        UI::Widgets::cellText(UI::Format::formatBytesPerSec(process.netSentBytesPerSec + process.netReceivedBytesPerSec));
    }
    return action;
}

} // namespace

std::optional<UsersViewAction>
renderUsersView(const Domain::UsersPublication* publication, bool hasSessions, bool canAct, UsersViewState& state)
{
    if (!hasSessions)
    {
        UI::Widgets::renderEmptyState(ICON_FA_USERS "  Users aren't available",
                                      "TaskSmack lists the signed-in users and their sessions on Linux. Support for Windows is planned.");
        return std::nullopt;
    }
    if (publication == nullptr || publication->version == 0)
    {
        UI::Widgets::renderEmptyState(ICON_FA_USERS "  Reading users...");
        return std::nullopt;
    }
    if (publication->status == Platform::UserSessionsStatus::Failed)
    {
        const std::string detail = "The signed-in sessions couldn't be read: " + publication->detail;
        UI::Widgets::renderEmptyState(ICON_FA_TRIANGLE_EXCLAMATION "  Users couldn't be read", detail.c_str());
        return std::nullopt;
    }
    if (state.rows.empty())
    {
        UI::Widgets::renderEmptyState(ICON_FA_USERS "  No one is signed in");
        return std::nullopt;
    }

    const auto nowUsec = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const std::optional<ImGuiID> menuId = canAct ? std::optional<ImGuiID>(ImGui::GetID(SESSION_MENU_ID)) : std::nullopt;
    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##UsersTable", 7, TABLE_FLAGS))
    {
        return std::nullopt;
    }
    const float em = ImGui::GetFontSize();
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("User", ImGuiTableColumnFlags_None, em * 18.0F);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_None, em * 9.0F);
    ImGui::TableSetupColumn("Processes", ImGuiTableColumnFlags_None, em * 6.0F);
    ImGui::TableSetupColumn("\xCE\xA3 CPU", ImGuiTableColumnFlags_None, em * 6.0F); // Σ: totals, never one process's (#838)
    ImGui::TableSetupColumn("\xCE\xA3 Memory", ImGuiTableColumnFlags_None, em * 7.0F);
    ImGui::TableSetupColumn("\xCE\xA3 Disk", ImGuiTableColumnFlags_None, em * 7.0F);
    ImGui::TableSetupColumn("\xCE\xA3 Network", ImGuiTableColumnFlags_None, em * 7.0F);
    ImGui::TableHeadersRow();

    std::optional<UsersViewAction> action;
    for (const UserRow& row : state.rows)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(row.user.c_str());
        const std::string label = std::string(ICON_FA_USER "  ") + row.user + (row.current ? "  (you)" : "");
        const bool open =
            ImGui::TreeNodeEx("##user", ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_DefaultOpen, "%s", label.c_str());
        ImGui::TableNextColumn();
        const std::size_t count = row.sessions.size();
        UI::Widgets::cellText(count == 1 ? std::string("1 session") : std::format("{} sessions", count));
        totalsCells(row.totals);
        if (open)
        {
            if (const auto clicked = renderUserChildren(row, state, menuId, nowUsec))
            {
                action = clicked;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();

    if (menuId.has_value() && ImGui::BeginPopup(SESSION_MENU_ID))
    {
        if (state.menuSession.has_value())
        {
            if (ImGui::MenuItem(ICON_FA_LOCK "  Lock"))
            {
                action = UsersViewAction{
                    .selectPid = 0, .selectStartTimeTicks = 0, .showDetails = false, .lock = state.menuSession, .signOut = std::nullopt};
            }
            ImGui::SetItemTooltip("Lock the session's screen; its user signs back in to unlock it");
            if (ImGui::MenuItem(ICON_FA_RIGHT_FROM_BRACKET "  Sign out..."))
            {
                action = UsersViewAction{
                    .selectPid = 0, .selectStartTimeTicks = 0, .showDetails = false, .lock = std::nullopt, .signOut = state.menuSession};
            }
            ImGui::SetItemTooltip("End the session and every program in it");
        }
        ImGui::EndPopup();
    }
    return action;
}

} // namespace App
