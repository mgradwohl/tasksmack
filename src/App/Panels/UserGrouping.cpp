#include "UserGrouping.h"

#include "App/Panels/ProcessGroupTotals.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/AsciiCase.h"
#include "Platform/IUserSessions.h"
#include "UI/Format.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace App::UserGrouping
{

namespace
{

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

std::string processAccountKey(const Domain::ProcessSnapshot& proc)
{
    if (proc.user.empty() || proc.userDomain.empty())
    {
        return proc.user;
    }
    std::string key;
    key.reserve(proc.userDomain.size() + 1 + proc.user.size());
    for (const char ch : proc.userDomain)
    {
        key += Platform::foldAscii(ch);
    }
    key += '\\';
    for (const char ch : proc.user)
    {
        key += Platform::foldAscii(ch);
    }
    return key;
}

std::vector<ProcessGrouping::Group> buildUserGroups(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                                    const std::vector<std::size_t>& order,
                                                    std::size_t liveCount,
                                                    ProcessColumn sortColumn,
                                                    bool ascending)
{
    using ProcessGrouping::Group;
    std::vector<Group> groups;
    std::unordered_map<std::string, std::size_t> byKey; // processAccountKey() -> its group
    // Each group's names as its lowest-numbered process spells them, whatever the table's order: its label.
    std::vector<AccountLess::Names> names;
    std::vector<std::size_t> namedBy; // the snapshot index `names` came from
    // Short names counted ignoring ASCII case, as Windows compares them (#3377 review): which spelling of an
    // account the table meets first can't change which account a process with no domain joins.
    std::map<std::string_view, std::size_t, NameLess> accountsNamed; // short name -> how many accounts with a domain have it
    const auto groupOf = [&](std::size_t index) -> std::size_t
    {
        const Domain::ProcessSnapshot& proc = snapshots[index];
        std::string key = processAccountKey(proc);
        auto [slot, added] = byKey.try_emplace(key, groups.size());
        if (added)
        {
            groups.push_back(Group{.key = std::move(key), .label = {}, .totals = {}, .members = {}});
            names.emplace_back(proc.userDomain, proc.user);
            namedBy.push_back(index);
            if (!proc.user.empty() && !proc.userDomain.empty())
            {
                ++accountsNamed[proc.user];
            }
        }
        else if (index < namedBy[slot->second])
        {
            names[slot->second] = AccountLess::Names{proc.userDomain, proc.user};
            namedBy[slot->second] = index;
        }
        return slot->second;
    };
    // First the accounts the processes name with a domain (Windows), so one that names none can join one.
    for (const std::size_t index : order)
    {
        if (index < snapshots.size() && !snapshots[index].user.empty() && !snapshots[index].userDomain.empty())
        {
            static_cast<void>(groupOf(index));
        }
    }
    // A short name one account has, in any case -> its group. Linux's processes name no domain, so it is
    // empty there and their groups stay keyed by the name as it is.
    std::map<std::string_view, std::size_t, NameLess> soleAccountNamed;
    for (std::size_t g = 0; g < groups.size(); ++g)
    {
        if (accountsNamed[names[g].second] == 1)
        {
            soleAccountNamed.emplace(names[g].second, g);
        }
    }
    for (const std::size_t index : order)
    {
        if (index >= snapshots.size())
        {
            continue;
        }
        const Domain::ProcessSnapshot& proc = snapshots[index];
        std::size_t slot = 0;
        if (const auto sole = soleAccountNamed.find(proc.user); proc.userDomain.empty() && sole != soleAccountNamed.end())
        {
            slot = sole->second; // Its domain wasn't read, and only one account has its name
        }
        else
        {
            slot = groupOf(index);
        }
        Group& group = groups[slot];
        group.members.push_back(index);
        if (index < liveCount)
        {
            group.totals.add(proc);
        }
    }
    // The short name, or DOMAIN\user when two groups share it, in any case (only a group with a domain gets
    // one, so Linux's alice and Alice stay alice and Alice).
    std::map<std::string_view, std::size_t, NameLess> groupsNamed;
    for (const AccountLess::Names& account : names)
    {
        ++groupsNamed[account.second];
    }
    for (std::size_t g = 0; g < groups.size(); ++g)
    {
        const auto& [domain, user] = names[g];
        groups[g].label = !domain.empty() && groupsNamed[user] > 1 ? std::string(domain) + "\\" + std::string(user) : std::string(user);
    }
    ProcessGrouping::sortGroups(groups, sortColumn, ascending);
    return groups;
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

std::string signOutQuestion(const Platform::UserSession& session)
{
    std::string question = std::format(
        "Sign out {} from {}?\n\nEvery program in the session ends, and unsaved work is lost.", session.user, sessionPlace(session));
    if (session.current)
    {
        question += " This is the session TaskSmack runs in, so TaskSmack closes too.";
    }
    return question;
}

std::string disconnectQuestion(const Platform::UserSession& session)
{
    if (session.current && session.remote)
    {
        // Inside Remote Desktop, Disconnect closes the connection rather than locking a screen.
        return std::format("Disconnect {} from {}?\n\nThis is the session TaskSmack runs in: the Remote Desktop connection closes. "
                           "Its programs, TaskSmack too, keep running until you reconnect.",
                           session.user,
                           sessionPlace(session));
    }
    if (session.current)
    {
        return std::format("Disconnect {} from {}?\n\nThis is the session TaskSmack runs in: this screen locks. Its programs, "
                           "TaskSmack too, keep running until you sign back in.",
                           session.user,
                           sessionPlace(session));
    }
    return std::format("Disconnect {} from {}?\n\nTheir programs keep running, and they sign back in to return to them.",
                       session.user,
                       sessionPlace(session));
}

} // namespace App::UserGrouping
