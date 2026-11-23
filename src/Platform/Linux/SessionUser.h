#pragma once

// Whom a TaskSmack run as root for another user should open links and folders as (#2461). Run with sudo
// (as the elevation notice suggests) or pkexec, a browser or file manager started as root either refuses
// to run (Firefox, Chrome, Dolphin) or opens as root on the user's display. So xdg-open is started as the
// user sudo or pkexec names, with that user's session environment. Pure: the environment, the passwd
// lookup and the file checks are passed in, so it is tested without being root.

#include "DetachedSpawn.h"
#include "Platform/SessionAccount.h"
#include "UserNameLookup.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <elf.h> // AT_SECURE
#include <grp.h>
#include <pwd.h>
#include <sys/auxv.h> // getauxval()
#include <sys/types.h>
#include <unistd.h>

namespace Platform::SessionUser
{

/// What the passwd entry gives of a user.
struct Account
{
    gid_t gid = 0;
    std::string name;
    std::string home;
    std::string shell;
    std::vector<gid_t> groups; ///< Every group the user is in, the primary included (getgrouplist()); empty: unknown
};

/// Variables kept from TaskSmack's own environment: how to reach the user's display and their locale, every one of
/// glibc's locale categories included (#4336: LC_CTYPE, LC_TIME and the rest were dropped, so xdg-open and the
/// programs it starts lost the user's character set and formats; RootEnvironment keeps the whole LC_ prefix).
/// Everything else (LD_PRELOAD, SUDO_*, root's HOME) is dropped.
inline constexpr std::array<std::string_view, 24> KEPT_VARIABLES{
    "PATH",
    "DISPLAY",
    "WAYLAND_DISPLAY",
    "XAUTHORITY",
    "LANG",
    "LANGUAGE",
    "LC_ALL",
    "LC_CTYPE",
    "LC_NUMERIC",
    "LC_TIME",
    "LC_COLLATE",
    "LC_MONETARY",
    "LC_MESSAGES",
    "LC_PAPER",
    "LC_NAME",
    "LC_ADDRESS",
    "LC_TELEPHONE",
    "LC_MEASUREMENT",
    "LC_IDENTIFICATION",
    "TZ",
    "XDG_SESSION_TYPE",
    "XDG_CURRENT_DESKTOP",
    "XDG_DATA_DIRS",
    "DESKTOP_SESSION",
};

/// A user sudo or pkexec names and their passwd entry.
struct InvokingUser
{
    uid_t uid = 0;
    Account account;
};

/// The user an elevated TaskSmack was started for, in the same cases as invokingUserSession(): nullopt when not root
/// (@p effectiveUid isn't 0), for root's own session (no SUDO_UID or PKEXEC_UID, or one naming root), or for a user
/// the passwd database doesn't know. SUDO_UID is asked first.
[[nodiscard]] inline std::optional<InvokingUser> invokingUser(uid_t effectiveUid,
                                                              const std::function<std::optional<std::string>(std::string_view)>& env,
                                                              const std::function<std::optional<Account>(uid_t)>& account)
{
    if (effectiveUid != 0)
    {
        return std::nullopt;
    }
    std::optional<std::string> uidText = env("SUDO_UID");
    if (!uidText.has_value())
    {
        uidText = env("PKEXEC_UID");
    }
    std::uint32_t uid = 0;
    if (!uidText.has_value() || uidText->empty() ||
        std::from_chars(uidText->data(), uidText->data() + uidText->size(), uid).ptr != uidText->data() + uidText->size() || uid == 0)
    {
        return std::nullopt;
    }
    std::optional<Account> user = account(static_cast<uid_t>(uid));
    if (!user.has_value())
    {
        return std::nullopt;
    }
    return InvokingUser{.uid = static_cast<uid_t>(uid), .account = std::move(*user)};
}

/// The user an elevated TaskSmack was started for and their session environment, or nullopt to start
/// programs as TaskSmack itself: not root (@p effectiveUid isn't 0), root's own session (no SUDO_UID or
/// PKEXEC_UID, or one naming root), or a user the passwd database doesn't know.
/// @p env looks up one of TaskSmack's variables; @p account, a user's passwd entry; @p exists, a path.
[[nodiscard]] inline std::optional<DetachedSpawn::RunAs>
invokingUserSession(uid_t effectiveUid,
                    const std::function<std::optional<std::string>(std::string_view)>& env,
                    const std::function<std::optional<Account>(uid_t)>& account,
                    const std::function<bool(const std::string&)>& exists)
{
    const std::optional<InvokingUser> invoking = invokingUser(effectiveUid, env, account);
    if (!invoking.has_value())
    {
        return std::nullopt;
    }
    const uid_t uid = invoking->uid;
    const Account& user = invoking->account;

    DetachedSpawn::RunAs runAs{.uid = uid, .gid = user.gid, .environment = {}, .groups = user.groups};
    std::vector<std::string>& environment = runAs.environment;
    environment.push_back("HOME=" + user.home);
    environment.push_back("USER=" + user.name);
    environment.push_back("LOGNAME=" + user.name);
    if (!user.shell.empty())
    {
        environment.push_back("SHELL=" + user.shell);
    }
    for (const std::string_view name : KEPT_VARIABLES)
    {
        if (const std::optional<std::string> value = env(name); value.has_value())
        {
            environment.push_back(std::string(name) + "=" + *value);
        }
    }
    // The user's own runtime directory and session bus, whatever sudo -E passed through: xdg-open asks the
    // desktop over D-Bus, and a relative WAYLAND_DISPLAY is found in XDG_RUNTIME_DIR.
    const std::string runtime = "/run/user/" + std::to_string(uid);
    if (exists(runtime))
    {
        environment.push_back("XDG_RUNTIME_DIR=" + runtime);
        if (exists(runtime + "/bus"))
        {
            environment.push_back("DBUS_SESSION_BUS_ADDRESS=unix:path=" + runtime + "/bus");
        }
    }
    return runAs;
}

/// One of this process's environment variables, or nullopt when it isn't set.
[[nodiscard]] inline std::optional<std::string> processVariable(std::string_view name)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - nothing changes the environment once main() starts its threads, so getenv() on a worker is safe (#4434)
    const char* const value = std::getenv(std::string(name).c_str());
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
}

/// The user's own values of the variables a root session replaced for itself (RootEnvironment: PATH, XDG_DATA_DIRS,
/// XAUTHORITY; the second #4187 review), kept for the programs it starts as that user, which run as them, so their own
/// values are safe there and needed (snap and Flatpak handlers, their ~/.local/bin, their Xauthority).
[[nodiscard]] inline std::map<std::string, std::optional<std::string>, std::less<>>& userSideValues()
{
    // nullopt: the user's session had none, and the program started as them gets none (#4259), never root's value.
    static std::map<std::string, std::optional<std::string>, std::less<>> values;
    return values;
}

/// processVariable(), or the user's own value of a variable a root session replaced (userSideValues()).
[[nodiscard]] inline std::optional<std::string> userSideVariable(std::string_view name)
{
    if (const auto found = userSideValues().find(name); found != userSideValues().end())
    {
        return found->second;
    }
    return processVariable(name);
}

/// Every group @p name is in, with @p gid (its primary group) among them, as a login sets them (getgrouplist());
/// empty when the database can't say.
[[nodiscard]] inline std::vector<gid_t> userGroups(const std::string& name, gid_t gid)
{
    constexpr int MOST_GROUPS = 65536; // NGROUPS_MAX on Linux
    int count = 32;
    std::vector<gid_t> groups;
    while (count > 0 && count <= MOST_GROUPS)
    {
        groups.resize(static_cast<std::size_t>(count));
        int found = count;
        if (::getgrouplist(name.c_str(), gid, groups.data(), &found) >= 0)
        {
            groups.resize(static_cast<std::size_t>(found));
            return groups;
        }
        count = found > count ? found : count * 2; // too few slots: found says how many it needs
    }
    return {};
}

/// @p uid's passwd entry, with its groups, or nullopt for a user the database doesn't know or one with no home
/// directory.
[[nodiscard]] inline std::optional<Account> systemAccount(uid_t uid)
{
    // The buffer grows for a large LDAP/SSSD entry, rather than opening links as root (#2499).
    std::optional<Account> account =
        UserNameLookupDetail::lookUpEntry<passwd>(uid,
                                                  ::getpwuid_r,
                                                  [](const passwd& entry) -> std::optional<Account>
                                                  {
                                                      if (entry.pw_dir == nullptr)
                                                      {
                                                          return std::nullopt;
                                                      }
                                                      return Account{
                                                          .gid = entry.pw_gid,
                                                          .name = entry.pw_name != nullptr ? entry.pw_name : "",
                                                          .home = entry.pw_dir,
                                                          .shell = entry.pw_shell != nullptr ? entry.pw_shell : "",
                                                          .groups = {},
                                                      };
                                                  });
    // Its supplementary groups (#3298): a program started as the user has them all, as in their own session.
    if (account.has_value() && !account->name.empty())
    {
        account->groups = userGroups(account->name, account->gid);
    }
    return account;
}

/// Whose TaskSmack this is (#3682, Linux's half of #3671): an Administrator session (#3580) started for a user runs as
/// root, so the per-user startup it would read and write (~/.config/autostart, systemd user units, crontab) is root's,
/// not that user's, and is left alone. Another account exactly when invokingUser() names a user (@p effectiveUid 0, and
/// SUDO_UID or PKEXEC_UID naming a user the passwd database knows); its name is @p effectiveUid's ("root"), or
/// "root" when the database has none. Never from a command-line value. Root's own session, and any other run, is the
/// same account.
[[nodiscard]] inline SessionAccount sessionAccount(uid_t effectiveUid,
                                                   const std::function<std::optional<std::string>(std::string_view)>& env,
                                                   const std::function<std::optional<Account>(uid_t)>& account)
{
    if (!invokingUser(effectiveUid, env, account).has_value())
    {
        return {};
    }
    const std::optional<Account> self = account(effectiveUid);
    return {.otherAccount = true, .account = self.has_value() && !self->name.empty() ? self->name : std::string("root")};
}

/// A process's parent and real and effective uids, as /proc/<pid>/status gives them.
struct ProcessIds
{
    int parent = 0;
    std::uint32_t realUid = 0;
    std::uint32_t effectiveUid = 0;
};

/// /proc/<@p pid>/status's PPid and Uid lines, or nullopt when it can't be read (the process is gone).
[[nodiscard]] inline std::optional<ProcessIds> processIds(int pid)
{
    std::ifstream in("/proc/" + std::to_string(pid) + "/status");
    std::optional<int> parent;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> uids;
    for (std::string line; std::getline(in, line);)
    {
        std::istringstream fields(line);
        std::string key;
        fields >> key;
        if (key == "PPid:")
        {
            int value = 0;
            if (fields >> value)
            {
                parent = value;
            }
        }
        else if (key == "Uid:")
        {
            std::uint32_t real = 0;
            std::uint32_t effective = 0;
            if (fields >> real >> effective)
            {
                uids = std::pair(real, effective);
            }
        }
    }
    if (!parent.has_value() || !uids.has_value())
    {
        return std::nullopt;
    }
    return ProcessIds{.parent = *parent, .realUid = uids->first, .effectiveUid = uids->second};
}

/// Whether a process above @p self, up to pid 1, runs as someone other than root (real or effective uid not 0), from
/// @p ids (processIds(), or a test's). su, sudo, doas and pkexec are run by a user, so a root process they start has
/// that user above it, whatever its environment says; a root login, a service, cron, wsl -u root and a container's
/// processes have only root above them. An ancestor that can't be read ends the walk (nothing more is known).
[[nodiscard]] inline bool aUserIsAbove(int self, const std::function<std::optional<ProcessIds>(int)>& ids)
{
    constexpr int MAX_HOPS = 64; // a chain this long is no shell's; and a loop can't keep the walk going
    std::optional<ProcessIds> at = ids(self);
    for (int hop = 0; hop < MAX_HOPS && at.has_value() && at->parent > 1; ++hop)
    {
        at = ids(at->parent);
        if (at.has_value() && (at->realUid != 0 || at->effectiveUid != 0))
        {
            return true;
        }
    }
    return false;
}

/// The real uid of the nearest process above @p self that isn't root's, up to pid 1, from @p ids (processIds(), or a
/// test's), or nullopt when there's none or an ancestor can't be read. The user who ran sudo, doas, su or pkexec is
/// that process (sudo's and su's front process keeps their real uid), so a root process it starts finds them first.
[[nodiscard]] inline std::optional<std::uint32_t> nearestUserAbove(int self, const std::function<std::optional<ProcessIds>(int)>& ids)
{
    constexpr int MAX_HOPS = 64; // as aUserIsAbove()
    std::optional<ProcessIds> at = ids(self);
    for (int hop = 0; hop < MAX_HOPS && at.has_value() && at->parent > 0; ++hop)
    {
        at = ids(at->parent);
        if (at.has_value() && at->realUid != 0)
        {
            return at->realUid;
        }
    }
    return std::nullopt;
}

/// Whether @p uid is the user who started @p self (#4186): nearestUserAbove(), by real uid only. Any ancestor of theirs
/// further up isn't enough: a user's shell an administrator reached with su, or a setuid helper, sits between (the
/// #4193 review).
[[nodiscard]] inline bool uidIsAbove(int self, uid_t uid, const std::function<std::optional<ProcessIds>(int)>& ids)
{
    return nearestUserAbove(self, ids) == static_cast<std::uint32_t>(uid);
}

/// @p value of the environment variable @p name as a root session may trust it (#4186): a SUDO_UID or PKEXEC_UID naming
/// a user only when @p isAbove says that user is the nearest one above this process, as sudo's and pkexec's caller is; under
/// su -p or doas keepenv the environment is the user's own, and could name anyone. Nullopt then. Any other variable,
/// and a value invokingUser() refuses anyway (malformed, or root), is @p value unchanged.
[[nodiscard]] inline std::optional<std::string>
invokerVariable(std::string_view name, std::optional<std::string> value, const std::function<bool(uid_t)>& isAbove)
{
    if ((name != "SUDO_UID" && name != "PKEXEC_UID") || !value.has_value() || value->empty())
    {
        return value;
    }
    std::uint32_t uid = 0;
    if (std::from_chars(value->data(), value->data() + value->size(), uid).ptr != value->data() + value->size() || uid == 0)
    {
        return value;
    }
    return isAbove(static_cast<uid_t>(uid)) ? std::move(value) : std::nullopt;
}

/// Whether this root process may trust a SUDO_UID or PKEXEC_UID naming @p uid: invokerVariable()'s check, against this
/// process's ancestors (uidIsAbove() over /proc).
[[nodiscard]] inline bool invokerIsAboveThisProcess(uid_t uid)
{
    return uidIsAbove(::getpid(), uid, &processIds);
}

/// processVariable(), with SUDO_UID and PKEXEC_UID as invokerVariable() trusts them, worked out once at first use
/// (startup: the config folder asks), while the process that ran sudo or pkexec is still there to see.
[[nodiscard]] inline std::optional<std::string> trustedProcessVariable(std::string_view name)
{
    if (name == "SUDO_UID")
    {
        static const std::optional<std::string> sudoUid = invokerVariable(name, processVariable(name), &invokerIsAboveThisProcess);
        return sudoUid;
    }
    if (name == "PKEXEC_UID")
    {
        static const std::optional<std::string> pkexecUid = invokerVariable(name, processVariable(name), &invokerIsAboveThisProcess);
        return pkexecUid;
    }
    return processVariable(name);
}

/// userSideVariable(), with SUDO_UID and PKEXEC_UID from trustedProcessVariable().
[[nodiscard]] inline std::optional<std::string> trustedUserSideVariable(std::string_view name)
{
    if (name == "SUDO_UID" || name == "PKEXEC_UID")
    {
        return trustedProcessVariable(name);
    }
    return userSideVariable(name);
}

/// Whether this process's environment may be another, less-privileged user's (#4154), so it ignores the test hooks that
/// write files (#4104) and the SDL and loader variables (#4111, #4151). No passwd lookup, and no signal the user's own
/// environment can set to turn it off:
///   - @p secureExecution, the kernel's AT_SECURE: setuid, setgid or file capabilities (the FAQ's setcap, #4171);
///   - not root but holding capabilities (@p holdsCapabilities: ambient ones, which set no AT_SECURE);
///   - a SUDO_UID or PKEXEC_UID that isn't this effective uid (sudo -u another user too), even a malformed one: only
///     sudo and pkexec set them;
///   - root with a DOAS_USER but root;
///   - root with a user's process above it (@p aUserAbove: aUserIsAbove()), which su, and sudo with SUDO_UID=0 given
///     on its command line, can't hide.
/// Root's own session (a root login, a service, cron, a container) is unaffected. Not HOME (release.yml's upgrade check
/// runs as root with HOME a scratch folder), nor the login uid (unset on WSL, and a user's inside a container they
/// started): the reviews of #4170.
[[nodiscard]] inline bool environmentMayBeAnotherUsers(uid_t effectiveUid,
                                                       bool secureExecution,
                                                       bool holdsCapabilities,
                                                       const std::function<std::optional<std::string>(std::string_view)>& env,
                                                       bool aUserAbove)
{
    if (secureExecution || (effectiveUid != 0 && holdsCapabilities))
    {
        return true;
    }
    const std::string self = std::to_string(effectiveUid);
    for (const char* name : {"SUDO_UID", "PKEXEC_UID"})
    {
        if (const std::optional<std::string> text = env(name); text.has_value() && !text->empty() && *text != self)
        {
            return true;
        }
    }
    if (effectiveUid != 0)
    {
        return false;
    }
    if (const std::optional<std::string> doasUser = env("DOAS_USER"); doasUser.has_value() && !doasUser->empty() && *doasUser != "root")
    {
        return true;
    }
    return aUserAbove;
}

/// Whether this process, not root, holds any effective or permitted capability (/proc/self/status CapEff, CapPrm).
[[nodiscard]] inline bool processHoldsCapabilities()
{
    std::ifstream in("/proc/self/status");
    for (std::string line; std::getline(in, line);)
    {
        if (line.starts_with("CapEff:") || line.starts_with("CapPrm:"))
        {
            const std::string_view hex = std::string_view(line).substr(line.find_first_not_of(" \t", 7));
            if (hex.find_first_not_of('0') != std::string_view::npos)
            {
                return true;
            }
        }
    }
    return false;
}

/// Whether this process is a capabilities session (#4766): not root, but holding capabilities, as the FAQ's setcap gives
/// it. It sees and acts on every process, but systemd decides service actions by user, not capability, so those still
/// need root. Worked out once.
[[nodiscard]] inline bool capabilitiesOnlySession()
{
    static const bool capabilitiesOnly = ::geteuid() != 0 && processHoldsCapabilities();
    return capabilitiesOnly;
}

/// Whether this process holds an ambient capability (/proc/self/status CapAmb), which a program it starts inherits; true
/// when that can't be read, so no child is assumed to hold none.
[[nodiscard]] inline bool processHoldsAmbientCapabilities()
{
    std::ifstream in("/proc/self/status");
    for (std::string line; std::getline(in, line);)
    {
        if (line.starts_with("CapAmb:"))
        {
            const std::string_view hex = std::string_view(line).substr(line.find_first_not_of(" \t", 7));
            return hex.find_first_not_of('0') != std::string_view::npos;
        }
    }
    return true;
}

/// Whether a program a non-root session starts as itself may get back the environment the session started with (#4651):
/// only when the child runs as the very user whose environment it is, holding nothing more than they do. So not root,
/// the real and effective uid and gid the same (a setuid or setgid TaskSmack's environment is its invoker's, and its
/// children run as the set ids), no ambient capability (a child inherits those; a setcap session's file capabilities
/// don't cross execve), and no SUDO_UID or PKEXEC_UID naming another uid (sudo -u: the environment is the invoker's),
/// a malformed one included. A setcap session (the FAQ's cap_sys_ptrace) is the case it's for: its programs, xdg-open's
/// browser and Run new task's, are the user's own, and get their GTK input method, GIO modules and shader cache back.
[[nodiscard]] inline bool childMayGetStartingEnvironment(uid_t realUid,
                                                         uid_t effectiveUid,
                                                         gid_t realGid,
                                                         gid_t effectiveGid,
                                                         bool ambientCapabilities,
                                                         const std::function<std::optional<std::string>(std::string_view)>& env)
{
    if (effectiveUid == 0 || realUid != effectiveUid || realGid != effectiveGid || ambientCapabilities)
    {
        return false;
    }
    const std::string self = std::to_string(effectiveUid);
    return std::ranges::all_of(std::array<const char*, 2>{"SUDO_UID", "PKEXEC_UID"},
                               [&env, &self](const char* name)
                               {
                                   const std::optional<std::string> text = env(name);
                                   return !text.has_value() || text->empty() || *text == self;
                               });
}

/// childMayGetStartingEnvironment() for this process, worked out once.
[[nodiscard]] inline bool thisProcessChildrenMayGetStartingEnvironment()
{
    static const bool may = childMayGetStartingEnvironment(
        ::getuid(), ::geteuid(), ::getgid(), ::getegid(), processHoldsAmbientCapabilities(), &processVariable);
    return may;
}

/// environmentMayBeAnotherUsers() for this process, worked out once (#4154): the rule for the writing hooks (#4104),
/// the SDL and loader scrub (#4111, #4151) and the config folder (#4172), in one place.
[[nodiscard]] inline bool thisProcessMayHaveAnotherUsersEnvironment()
{
    static const bool mayBe = []
    {
        const auto effective = ::geteuid(); // uid_t
        return environmentMayBeAnotherUsers(effective,
                                            ::getauxval(AT_SECURE) != 0,
                                            effective != 0 && processHoldsCapabilities(),
                                            &processVariable,
                                            effective == 0 && aUserIsAbove(::getpid(), &processIds));
    }();
    return mayBe;
}

/// This process's invoking user (invokingUser() for its effective uid, environment and passwd database), looked up
/// once (#3773): getpwuid_r() and getgrouplist() can go over LDAP or SSSD, and every open-link action asks. Neither
/// the uid nor SUDO_UID/PKEXEC_UID changes during a run, and the account is what it was at startup for the run's
/// whole life, as the settings folder chosen from it is. Thread-safe (a function-local static).
[[nodiscard]] inline const std::optional<InvokingUser>& processInvokingUser()
{
    static const std::optional<InvokingUser> invoking = invokingUser(::geteuid(), &trustedProcessVariable, &systemAccount);
    return invoking;
}

/// systemAccount(), with the invoking user's account answered from processInvokingUser() rather than looked up again.
[[nodiscard]] inline std::optional<Account> processAccount(uid_t uid)
{
    if (const std::optional<InvokingUser>& invoking = processInvokingUser(); invoking.has_value() && invoking->uid == uid)
    {
        return invoking->account;
    }
    return systemAccount(uid);
}

/// The home directory of the user this process was started for as root (#3580): processInvokingUser()'s. An
/// Administrator session keeps that user's settings there.
[[nodiscard]] inline std::optional<std::string> invokingUserHome()
{
    const std::optional<InvokingUser>& invoking = processInvokingUser();
    if (!invoking.has_value() || invoking->account.home.empty())
    {
        return std::nullopt;
    }
    return invoking->account.home;
}

/// The home whose settings this session keeps when its environment can't be trusted to name them (#4802): the user a
/// root session was started for (@p invokingHome, invokingUserHome(), #3580), or, in a capabilities session
/// (@p capabilitiesOnly, #4766), the real user's own, from @p realUserAccount (the passwd entry). That session runs with
/// the user's environment but reads past file permissions, so Platform::ConfigDirectory::configDirectoryFor() takes
/// XDG_CONFIG_HOME and HOME only inside this home. nullopt in a normal session, whose environment is its own; and in a
/// capabilities session whose account can't be read, where Platform::UserPermissionsOnly still keeps the reads the user's.
[[nodiscard]] inline std::optional<std::string> settingsOwnerHome(bool capabilitiesOnly,
                                                                  const std::optional<std::string>& invokingHome,
                                                                  const std::function<std::optional<Account>()>& realUserAccount)
{
    if (!capabilitiesOnly)
    {
        return invokingHome;
    }
    if (const std::optional<Account> account = realUserAccount ? realUserAccount() : std::nullopt;
        account.has_value() && !account->home.empty())
    {
        return account->home;
    }
    return std::nullopt;
}

/// settingsOwnerHome() for this process: capabilitiesOnlySession(), invokingUserHome() and the real uid's account.
[[nodiscard]] inline std::optional<std::string> settingsOwnerHome()
{
    return settingsOwnerHome(capabilitiesOnlySession(), invokingUserHome(), [] { return processAccount(::getuid()); });
}

/// invokingUserSession() for this process: its effective uid and environment, and the account looked up once
/// (processAccount(), #3773). Whether the user's runtime folder exists is asked each time.
[[nodiscard]] inline std::optional<DetachedSpawn::RunAs> invokingUserSession()
{
    return invokingUserSession(
        ::geteuid(), &trustedUserSideVariable, &processAccount, [](const std::string& path) { return ::access(path.c_str(), F_OK) == 0; });
}

} // namespace Platform::SessionUser
