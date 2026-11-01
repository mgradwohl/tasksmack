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
#include <string_view>
#include <utility>
#include <vector>

namespace App
{

namespace
{

constexpr const char* SESSION_MENU_ID = "##UserSessionMenu";
constexpr std::uint64_t USEC_PER_MINUTE = 60'000'000;
constexpr std::uint64_t USEC_PER_SECOND = 1'000'000;

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
    if (type == "console")
    {
        return "Console"; // Windows: the machine's own screen
    }
    if (type == "rdp")
    {
        return "Remote Desktop";
    }
    return type == "unspecified" ? std::string{} : type;
}

/// An account by domain and user name, ordered with ASCII case ignored, as Windows compares account names
/// (#3310). The key views the rows' names, so a process's names are looked up without building a string.
struct AccountLess
{
    using Names = std::pair<std::string_view, std::string_view>; // domain, user

    [[nodiscard]] bool operator()(const Names& a, const Names& b) const noexcept
    {
        const auto byDomain = Platform::compareIgnoreCase(a.first, b.first);
        return byDomain != 0 ? byDomain < 0 : Platform::compareIgnoreCase(a.second, b.second) < 0;
    }
};

/// A short name ordered with ASCII case ignored, for a Windows process's name (#3310).
struct NameLess
{
    [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept
    {
        return Platform::compareIgnoreCase(a, b) < 0;
    }
};

} // namespace

bool isSignInSession(const Platform::UserSession& session)
{
    return session.sessionClass.empty() || session.sessionClass == "user";
}

std::vector<UserRow> buildUserRows(const std::vector<Platform::UserSession>& sessions,
                                   const std::vector<Domain::ProcessSnapshot>& processes)
{
    std::vector<UserRow> rows;
    std::vector<std::string> domains;              // each row's domain, from its first session; empty on Linux
    std::map<std::string, std::size_t> byKey;      // account key -> row
    std::map<std::string, std::size_t> namesInUse; // short name -> how many rows have it
    for (const Platform::UserSession& session : sessions)
    {
        if (!isSignInSession(session))
        {
            continue;
        }
        const std::string key = Platform::accountKey(session);
        auto [found, added] = byKey.try_emplace(key, rows.size());
        if (added)
        {
            rows.push_back(UserRow{
                .user = session.unreadable.empty() ? session.user : std::string{},
                .key = key,
                .label = {},
                .uid = session.uid,
                .sessions = {},
                .totals = {},
                .processes = {},
                .current = false,
            });
            domains.push_back(rows.back().user.empty() ? std::string{} : session.domain);
            if (!rows.back().user.empty())
            {
                ++namesInUse[rows.back().user];
            }
        }
        UserRow& row = rows[found->second];
        row.current = row.current || session.current;
        row.sessions.push_back(session);
    }
    // A process that names its owner's domain (Windows, #3310) goes to the row of that domain and user. One
    // that names only a short name (Linux, or a Windows owner whose domain wasn't read) goes to the one row
    // with that name, never to one of two.
    std::map<AccountLess::Names, std::size_t, AccountLess> byAccount; // domain, user -> its row
    std::map<std::string, std::size_t> byName;                        // short name -> its one row
    // For a process with a domain, a row with none, when no other row has its name in any case: Windows
    // names ignore case, so "alice" beside CORP\\ALICE could be either.
    std::map<std::string_view, std::size_t, NameLess> namesInUseIgnoringCase;
    for (const UserRow& row : rows)
    {
        if (!row.user.empty())
        {
            ++namesInUseIgnoringCase[row.user];
        }
    }
    std::map<std::string_view, std::size_t, NameLess> byNameWithoutDomain;
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        UserRow& row = rows[i];
        const bool shared = !row.user.empty() && namesInUse[row.user] > 1;
        if (row.user.empty())
        {
            row.label = "Not readable";
        }
        else
        {
            row.label = shared ? row.key : row.user;
        }
        if (row.user.empty())
        {
            continue;
        }
        if (!domains[i].empty())
        {
            byAccount.emplace(AccountLess::Names{domains[i], row.user}, i);
        }
        if (!shared)
        {
            byName.emplace(row.user, i);
        }
        if (domains[i].empty() && namesInUseIgnoringCase[row.user] == 1)
        {
            byNameWithoutDomain.emplace(row.user, i);
        }
    }
    for (std::size_t i = 0; i < processes.size(); ++i)
    {
        const Domain::ProcessSnapshot& process = processes[i];
        std::optional<std::size_t> rowIndex;
        if (process.userDomain.empty())
        {
            if (const auto found = byName.find(process.user); found != byName.end())
            {
                rowIndex = found->second;
            }
        }
        else if (const auto found = byAccount.find(AccountLess::Names{process.userDomain, process.user}); found != byAccount.end())
        {
            rowIndex = found->second;
        }
        else if (const auto bare = byNameWithoutDomain.find(process.user); bare != byNameWithoutDomain.end())
        {
            rowIndex = bare->second; // A row whose sessions gave no domain can't be told from this one's
        }
        if (rowIndex)
        {
            UserRow& row = rows[*rowIndex];
            row.totals.add(process);
            row.processes.push_back(i);
        }
    }
    for (UserRow& row : rows)
    {
        std::ranges::stable_sort(row.processes,
                                 [&processes](std::size_t a, std::size_t b) { return processes[a].cpuPercent > processes[b].cpuPercent; });
    }
    std::ranges::stable_sort(rows, [](const UserRow& a, const UserRow& b) { return Platform::compareIgnoreCase(a.label, b.label) < 0; });
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
    if (session.state == "disconnected")
    {
        return "Disconnected"; // Windows: signed in, its programs running, no screen
    }
    if (session.state == "connecting")
    {
        return "Connecting";
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

std::string sessionTooltip(const Platform::UserSession& session)
{
    if (!session.unreadable.empty())
    {
        return "Its user couldn't be read: " + session.unreadable + "\nSession " + session.id;
    }
    std::string text = session.domain.empty() ? session.user : session.domain + "\\" + session.user;
    if (session.logonUsec != 0)
    {
        text += "\nSigned in " + UI::Format::formatEpochDateTimeShort(session.logonUsec / USEC_PER_SECOND);
    }
    text += "\nSession " + session.id;
    return text;
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
        const std::string place = session.unreadable.empty() ? sessionPlace(session) : "Session " + session.id + " (not readable)";
        const std::string label = std::string(ICON_FA_DESKTOP "  ") + place + (session.current ? "  (this session)" : "");
        ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) // built only when shown, not every frame
        {
            ImGui::SetTooltip("%s", sessionTooltip(session).c_str());
        }
        // Nothing is offered on a session not readable: who it belongs to can't be confirmed.
        if (menuId.has_value() && session.unreadable.empty() && ImGui::IsItemClicked(ImGuiMouseButton_Right))
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
            action = UsersViewAction{
                .selectPid = process.pid,
                .selectStartTimeTicks = process.startTimeTicks,
                .showDetails = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left),
                .lock = std::nullopt,
                .signOut = std::nullopt,
                .disconnect = std::nullopt,
            };
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

namespace
{

/// Draws the Users tab's empty state when there is no table to show, and says whether it did: the platform
/// can't list sessions, nothing has been read yet, the read failed (with why), or nobody is signed in.
[[nodiscard]] bool renderUsersEmptyState(const Domain::UsersPublication* publication, bool hasSessions, const UsersViewState& state)
{
    if (!hasSessions)
    {
        UI::Widgets::renderEmptyState(ICON_FA_USERS "  Users aren't available",
                                      "This run of TaskSmack can't list the signed-in users and their sessions.");
        return true;
    }
    if (publication == nullptr || publication->version == 0)
    {
        UI::Widgets::renderEmptyState(ICON_FA_USERS "  Reading users...");
        return true;
    }
    if (publication->status == Platform::UserSessionsStatus::Failed)
    {
        const std::string detail = "The signed-in sessions couldn't be read: " + publication->detail;
        UI::Widgets::renderEmptyState(ICON_FA_TRIANGLE_EXCLAMATION "  Users couldn't be read", detail.c_str());
        return true;
    }
    if (state.rows.empty())
    {
        UI::Widgets::renderEmptyState(ICON_FA_USERS "  No one is signed in");
        return true;
    }
    return false;
}

/// Begins the users table and sets up its seven columns: User (frozen), Status, Processes and the four Σ
/// totals. False when the table isn't visible, and then nothing is to be drawn or ended.
[[nodiscard]] bool beginUsersTable()
{
    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                                            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##UsersTable", 7, TABLE_FLAGS))
    {
        return false;
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
    return true;
}

/// One user's row (a tree node, open by default, with their session count and totals) and, while it is
/// open, their sessions and processes under it. What the user did in those rows, if anything.
[[nodiscard]] std::optional<UsersViewAction>
renderUserRow(const UserRow& row, UsersViewState& state, std::optional<ImGuiID> menuId, std::uint64_t nowUsec)
{
    std::optional<UsersViewAction> action;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(row.key.c_str());
    const std::string label = std::string(ICON_FA_USER "  ") + row.label + (row.current ? "  (you)" : "");
    const bool open = ImGui::TreeNodeEx("##user", ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_DefaultOpen, "%s", label.c_str());
    ImGui::TableNextColumn();
    const std::size_t count = row.sessions.size();
    UI::Widgets::cellText(count == 1 ? std::string("1 session") : std::format("{} sessions", count));
    totalsCells(row.totals);
    if (open)
    {
        action = renderUserChildren(row, state, menuId, nowUsec);
        ImGui::TreePop();
    }
    ImGui::PopID();
    return action;
}

/// The session row menu, for the session it was opened on (state.menuSession), with the items @p menu allows:
/// Lock (Linux), Disconnect... (Windows; greyed for a session already disconnected) and Sign out.... The
/// action chosen, if any; the panel runs it, asking first where it should.
[[nodiscard]] std::optional<UsersViewAction> renderSessionMenu(SessionMenu menu, const UsersViewState& state)
{
    if (!ImGui::BeginPopup(SESSION_MENU_ID))
    {
        return std::nullopt;
    }
    std::optional<UsersViewAction> action;
    if (state.menuSession.has_value())
    {
        const UsersViewAction none{
            .selectPid = 0,
            .selectStartTimeTicks = 0,
            .showDetails = false,
            .lock = std::nullopt,
            .signOut = std::nullopt,
            .disconnect = std::nullopt,
        };
        if (menu.lock && ImGui::MenuItem(ICON_FA_LOCK "  Lock"))
        {
            action = none;
            action->lock = state.menuSession;
        }
        if (menu.lock)
        {
            ImGui::SetItemTooltip("Lock the session's screen; its user signs back in to unlock it");
        }
        // Windows' Disconnect, in Lock's place (#3076): the session stays signed in, its programs running.
        const bool disconnected = state.menuSession->state == "disconnected";
        if (menu.disconnect && ImGui::MenuItem(ICON_FA_LINK_SLASH "  Disconnect...", nullptr, false, !disconnected))
        {
            action = none;
            action->disconnect = state.menuSession;
        }
        if (menu.disconnect)
        {
            ImGui::SetItemTooltip(disconnected ? "The session is already disconnected"
                                               : "Detach the session from its screen; its programs keep running until its user "
                                                 "signs back in");
        }
        if (menu.signOut && ImGui::MenuItem(ICON_FA_RIGHT_FROM_BRACKET "  Sign out..."))
        {
            action = none;
            action->signOut = state.menuSession;
        }
        if (menu.signOut)
        {
            ImGui::SetItemTooltip("End the session and every program in it");
        }
    }
    ImGui::EndPopup();
    return action;
}

} // namespace

std::optional<UsersViewAction>
renderUsersView(const Domain::UsersPublication* publication, bool hasSessions, SessionMenu menu, UsersViewState& state)
{
    // Nothing to list (unsupported, not read yet, failed, nobody signed in): the empty state says which.
    if (renderUsersEmptyState(publication, hasSessions, state))
    {
        return std::nullopt;
    }
    const auto nowUsec = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    // The row menu exists only where the platform can act; without one, a right-click opens nothing.
    const std::optional<ImGuiID> menuId = menu.any() ? std::optional<ImGuiID>(ImGui::GetID(SESSION_MENU_ID)) : std::nullopt;
    if (!beginUsersTable())
    {
        return std::nullopt;
    }
    std::optional<UsersViewAction> action;
    for (const UserRow& row : state.rows)
    {
        if (auto clicked = renderUserRow(row, state, menuId, nowUsec))
        {
            action = std::move(clicked);
        }
    }
    ImGui::EndTable();
    // The menu is drawn after the table so it stays open while the rows are rebuilt under it (#1365).
    if (menuId.has_value())
    {
        if (auto chosen = renderSessionMenu(menu, state))
        {
            action = std::move(chosen);
        }
    }
    return action;
}

} // namespace App
