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
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

namespace
{

/// "pc\\alice": @p domain and @p user, ASCII case folded, the key of an account that names its domain.
[[nodiscard]] std::string foldedAccountKey(std::string_view domain, std::string_view user)
{
    std::string key;
    key.reserve(domain.size() + 1 + user.size());
    for (const char ch : domain)
    {
        key += Platform::foldAscii(ch);
    }
    key += '\\';
    for (const char ch : user)
    {
        key += Platform::foldAscii(ch);
    }
    return key;
}

/// Whether @p session's user was read, so it belongs to an account (not the Not readable group).
[[nodiscard]] bool hasReadableUser(const Platform::UserSession& session) noexcept
{
    return session.unreadable.empty() && !session.user.empty();
}

} // namespace

std::string processAccountKey(const Domain::ProcessSnapshot& proc)
{
    if (proc.user.empty() || proc.userDomain.empty())
    {
        return proc.user;
    }
    return foldedAccountKey(proc.userDomain, proc.user);
}

std::vector<std::string> sharedUserNames(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    // A short name -> the first domain seen with it, and whether another domain has it too. The keys view
    // the snapshots' names, so a process of an account already seen costs a lookup and no allocation.
    struct Seen
    {
        std::string_view domain;
        bool shared = false;
    };
    std::map<std::string_view, Seen, NameLess> byName;
    for (const Domain::ProcessSnapshot& proc : snapshots)
    {
        if (proc.user.empty() || proc.userDomain.empty())
        {
            continue;
        }
        const auto [found, added] = byName.try_emplace(proc.user, Seen{.domain = proc.userDomain, .shared = false});
        if (!added && !Platform::equalsIgnoreCase(found->second.domain, proc.userDomain))
        {
            found->second.shared = true;
        }
    }
    std::vector<std::string> shared;
    for (const auto& [name, seen] : byName)
    {
        if (seen.shared)
        {
            shared.emplace_back(name); // In byName's order: sorted ignoring ASCII case
        }
    }
    return shared;
}

bool isSharedUserName(const std::vector<std::string>& shared, std::string_view user) noexcept
{
    if (user.empty())
    {
        return false;
    }
    const auto found = std::ranges::lower_bound(shared, user, NameLess{});
    return found != shared.end() && Platform::equalsIgnoreCase(*found, user);
}

std::vector<ProcessGrouping::Group> buildUserGroups(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                                    const std::vector<std::size_t>& order,
                                                    std::size_t liveCount,
                                                    ProcessColumn sortColumn,
                                                    bool ascending,
                                                    const std::vector<Platform::UserSession>& sessions)
{
    using ProcessGrouping::Group;
    // The accounts come from the whole generation, not from `order` (#3377 review): the filter hides rows,
    // not accounts, so a process with no domain is as ambiguous with PC\alice's process filtered out as with
    // it shown. Short names count ignoring ASCII case, as Windows compares them, so neither the table's order
    // nor which spelling it meets first changes the answer. A signed-in account counts too, processes or not,
    // as the Users tab's rows once did (#3310): a process with no domain joins the one account with its name.
    std::unordered_set<std::string> accounts;                        // processAccountKey() of each with a domain
    std::map<std::string_view, std::size_t, NameLess> accountsNamed; // short name -> how many accounts have it
    std::map<std::string_view, std::string, NameLess> accountNamed;  // short name -> an account's key with it
    const auto countAccount = [&](std::string_view domain, std::string_view user)
    {
        if (user.empty() || domain.empty())
        {
            return;
        }
        if (const auto [account, added] = accounts.insert(foldedAccountKey(domain, user)); added)
        {
            ++accountsNamed[user];
            accountNamed.try_emplace(user, *account);
        }
    };
    for (const Domain::ProcessSnapshot& proc : snapshots)
    {
        countAccount(proc.userDomain, proc.user);
    }
    for (const Platform::UserSession& session : sessions)
    {
        if (isSignInSession(session) && hasReadableUser(session))
        {
            countAccount(session.domain, session.user);
        }
    }
    // The key of the account @p domain and @p user name: its own, or, with no domain, the one account with the
    // name when only one has it, else the name alone.
    const auto keyOf = [&](std::string_view domain, std::string_view user) -> std::string
    {
        if (user.empty())
        {
            return {};
        }
        if (!domain.empty())
        {
            return foldedAccountKey(domain, user);
        }
        if (const auto sole = accountNamed.find(user); sole != accountNamed.end() && accountsNamed[user] == 1)
        {
            return sole->second; // Its domain wasn't read, and only one account has its name
        }
        return std::string(user);
    };
    // Then the rows the table shows, in its order: a group only for an account with one of them, or signed in.
    std::vector<Group> groups;
    std::unordered_map<std::string, std::size_t> byKey; // key -> its group
    // Each group's names as its lowest-numbered member spells them, whatever the table's order: its label.
    std::vector<AccountLess::Names> names;
    std::vector<std::size_t> namedBy; // the snapshot index `names` came from; NAMED_BY_SESSION for a session
    constexpr std::size_t NAMED_BY_SESSION = std::numeric_limits<std::size_t>::max();
    for (const std::size_t index : order)
    {
        if (index >= snapshots.size())
        {
            continue;
        }
        const Domain::ProcessSnapshot& proc = snapshots[index];
        std::string key = keyOf(proc.userDomain, proc.user);
        auto [slot, added] = byKey.try_emplace(key, groups.size());
        if (added)
        {
            groups.push_back(Group{
                .key = std::move(key),
                .label = {},
                .totals = {},
                .members = {},
                .sessions = {},
                .current = false,
                .notReadable = false,
            });
            names.emplace_back(proc.userDomain, proc.user);
            namedBy.push_back(index);
        }
        else if (index < namedBy[slot->second])
        {
            names[slot->second] = AccountLess::Names{proc.userDomain, proc.user};
            namedBy[slot->second] = index;
        }
        Group& group = groups[slot->second];
        group.members.push_back(index);
        if (index < liveCount)
        {
            group.totals.add(proc);
        }
    }
    // Each sign-in session under its account (#3377), which gets a group of its own when none of its processes
    // is shown: the filter hides processes, never who is signed in (the maintainer's decision). Sessions whose
    // user couldn't be read have no account to go to: they share one Not readable group.
    for (const Platform::UserSession& session : sessions)
    {
        if (!isSignInSession(session))
        {
            continue;
        }
        const bool readable = hasReadableUser(session);
        std::string key = readable ? keyOf(session.domain, session.user) : std::string(NOT_READABLE_GROUP_KEY);
        auto [slot, added] = byKey.try_emplace(key, groups.size());
        if (added)
        {
            groups.push_back(Group{
                .key = std::move(key),
                .label = {},
                .totals = {},
                .members = {},
                .sessions = {},
                .current = false,
                .notReadable = !readable,
            });
            names.emplace_back(session.domain, session.user);
            namedBy.push_back(NAMED_BY_SESSION);
        }
        Group& group = groups[slot->second];
        group.sessions.push_back(session);
        group.current = group.current || session.current;
    }
    // The short name, or DOMAIN\user when two accounts in the generation share it, in any case (only a group
    // with a domain gets one, so Linux's alice and Alice stay alice and Alice).
    for (std::size_t g = 0; g < groups.size(); ++g)
    {
        if (groups[g].notReadable)
        {
            groups[g].label = "Not readable";
            continue;
        }
        const auto& [domain, user] = names[g];
        const auto named = accountsNamed.find(user);
        const bool shared = named != accountsNamed.end() && named->second > 1;
        groups[g].label = !domain.empty() && shared ? std::string(domain) + "\\" + std::string(user) : std::string(user);
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

std::string groupHeaderName(const ProcessGrouping::Group& group)
{
    return group.current ? group.label + "  (you)" : group.label;
}

std::string groupStatus(const ProcessGrouping::Group& group, std::uint64_t nowUsec)
{
    if (group.sessions.size() == 1)
    {
        return group.notReadable ? std::string("1 session") : sessionStatus(group.sessions.front(), nowUsec);
    }
    return group.sessions.empty() ? std::string{} : std::format("{} sessions", group.sessions.size());
}

std::string groupHeaderTooltip(const ProcessGrouping::Group& group, bool collapsed)
{
    std::string tooltip = collapsed ? "Expand" : "Collapse";
    if (group.notReadable)
    {
        for (const Platform::UserSession& session : group.sessions)
        {
            tooltip += std::format(
                "\nSession {}: its user couldn't be read{}{}", session.id, session.unreadable.empty() ? "" : ": ", session.unreadable);
        }
    }
    else if (!group.sessions.empty())
    {
        tooltip += "\nThe \xCE\xA3 totals are of the processes TaskSmack can see"; // Σ
    }
    return tooltip;
}

const Platform::UserSession* headerMenuSession(const ProcessGrouping::Group& group) noexcept
{
    if (group.notReadable || group.sessions.size() != 1 || !hasReadableUser(group.sessions.front()))
    {
        return nullptr;
    }
    return &group.sessions.front();
}

std::string sessionRowName(const Platform::UserSession& session)
{
    std::string name = session.unreadable.empty() ? sessionPlace(session) : "Session " + session.id + " (not readable)";
    if (session.current)
    {
        name += "  (this session)";
    }
    return name;
}

std::string sessionSignInTime(const Platform::UserSession& session)
{
    return session.logonUsec == 0 ? std::string{} : UI::Format::formatEpochDateTimeShort(session.logonUsec / USEC_PER_SECOND);
}

} // namespace App::UserGrouping
