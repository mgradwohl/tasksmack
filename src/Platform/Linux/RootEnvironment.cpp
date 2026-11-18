#include "RootEnvironment.h"

#include "SessionUser.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h> // environ

namespace Platform::RootEnvironment
{

namespace
{

std::vector<std::string>& removedNames() noexcept
{
    static std::vector<std::string> names;
    return names;
}

/// root's home from its passwd entry, never HOME (the user's under sudo -E).
std::optional<std::string> rootPasswdHome()
{
    passwd entry{};
    passwd* found = nullptr;
    std::vector<char> buffer(16384);
    if (::getpwuid_r(0, &entry, buffer.data(), buffer.size(), &found) != 0 || found == nullptr || entry.pw_dir == nullptr ||
        entry.pw_dir[0] == '\0')
    {
        return std::nullopt;
    }
    return std::string(entry.pw_dir);
}

/// The uid that started this root session, from what sudo, pkexec or doas set (SUDO_UID, PKEXEC_UID, DOAS_USER), and
/// only when a process above this one runs as it (#4186), or nullopt (su names no one).
std::optional<uid_t> invokingUid()
{
    for (const char* name : {"SUDO_UID", "PKEXEC_UID"})
    {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) - first thing in main(), before any thread
        if (const char* text = std::getenv(name); text != nullptr && *text != '\0')
        {
            std::uint32_t uid = 0;
            const std::string_view view(text);
            if (std::from_chars(view.data(), view.data() + view.size(), uid).ptr == view.data() + view.size() && uid != 0 &&
                SessionUser::invokerIsAboveThisProcess(static_cast<uid_t>(uid))) // never one su -p passed through (#4186)
            {
                return static_cast<uid_t>(uid);
            }
        }
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - as above
    if (const char* doasUser = std::getenv("DOAS_USER"); doasUser != nullptr && *doasUser != '\0')
    {
        passwd entry{};
        passwd* found = nullptr;
        std::vector<char> buffer(16384);
        if (::getpwnam_r(doasUser, &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr && entry.pw_uid != 0 &&
            SessionUser::invokerIsAboveThisProcess(entry.pw_uid)) // as SUDO_UID (#4186)
        {
            return entry.pw_uid;
        }
    }
    return std::nullopt;
}

/// Whether the kernel's login uid (/proc/self/loginuid, set at login and not changeable by the user) agrees that
/// @p uid started this session: a SUDO_UID forged under su -p (#4186) names someone else. An unset login uid
/// (4294967295: no PAM login, as in containers) doesn't refuse.
bool loginUidAllows(uid_t uid)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const int file = ::open("/proc/self/loginuid", O_RDONLY | O_CLOEXEC);
    if (file < 0)
    {
        return true;
    }
    std::array<char, 16> text{};
    const auto got = ::read(file, text.data(), text.size() - 1);
    ::close(file);
    if (got <= 0)
    {
        return true;
    }
    std::uint32_t login = 0;
    const char* end = text.data() + got;
    if (std::from_chars(text.data(), end, login).ec != std::errc{})
    {
        return false;
    }
    constexpr std::uint32_t UNSET = 4294967295U;
    return login == UNSET || login == static_cast<std::uint32_t>(uid);
}

/// root's own copy of the invoking user's Xauthority (the second #4187 review): the file XAUTHORITY names, or their
/// ~/.Xauthority from their passwd entry, opened without following a link and used only if it's a plain file of
/// theirs (and the login uid agrees whose), so no other account's cookie can be read and sent to a display the user chose; copied 0600 into
/// root's runtime folder or /run/tasksmack, both root's alone. Nullopt when there's no such file, or no user known.
std::optional<std::string> rootXauthorityCopy(const std::optional<std::string>& rootRuntimeDir)
{
    const std::optional<uid_t> uid = invokingUid();
    if (!uid.has_value() || !loginUidAllows(*uid))
    {
        return std::nullopt;
    }
    std::string source;
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - first thing in main(), before any thread
    if (const char* named = std::getenv("XAUTHORITY"); named != nullptr && *named != '\0')
    {
        source = named;
    }
    else
    {
        passwd entry{};
        passwd* found = nullptr;
        std::vector<char> buffer(16384);
        if (::getpwuid_r(*uid, &entry, buffer.data(), buffer.size(), &found) != 0 || found == nullptr || entry.pw_dir == nullptr)
        {
            return std::nullopt;
        }
        source = std::string(entry.pw_dir) + "/.Xauthority";
    }
    // Opened as the user (filesystem uid and gid theirs for this one open; no thread runs yet), so a path the user
    // names reaches only what they could open themselves; never blocking on a FIFO (the CodeQL path alert on #4187).
    const int in = openAsUser(source, *uid);
    if (in < 0)
    {
        return std::nullopt;
    }
    const std::optional<std::string> contents = readOwnedFile(in, *uid, MOST_XAUTHORITY_BYTES);
    ::close(in);
    if (!contents.has_value() || contents->empty())
    {
        return std::nullopt;
    }
    const std::string folder = rootRuntimeDir.value_or("/run/tasksmack");
    if (!rootRuntimeDir.has_value())
    {
        static_cast<void>(::mkdir(folder.c_str(), S_IRWXU));
        if (!isPrivateFolderOf(folder, 0))
        {
            return std::nullopt;
        }
    }
    const std::string copy = folder + "/tasksmack-xauthority";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const int out = ::open(copy.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (out < 0)
    {
        return std::nullopt;
    }
    const bool written = ::fchmod(out, S_IRUSR | S_IWUSR) == 0 && ::write(out, contents->data(), contents->size()) ==
                                                                      static_cast<decltype(::write(out, nullptr, 0))>(contents->size());
    ::close(out);
    return written ? std::optional(copy) : std::nullopt;
}

} // namespace

int openAsUser(const std::string& path, uid_t uid)
{
    passwd entry{};
    passwd* found = nullptr;
    std::vector<char> buffer(16384);
    if (::getpwuid_r(uid, &entry, buffer.data(), buffer.size(), &found) != 0 || found == nullptr)
    {
        return -1;
    }
    // Put back after the open: the effective ids, which the filesystem ids follow unless something set them (#4255): root's
    // in a root session, a test's own otherwise.
    const auto previousGid = static_cast<int>(::getegid());
    const auto previousUid = static_cast<int>(::geteuid());
    static_cast<void>(::setfsgid(entry.pw_gid));
    static_cast<void>(::setfsuid(uid));
    const bool switched = ::setfsuid(static_cast<uid_t>(-1)) == static_cast<int>(uid);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const int file = switched ? ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC) : -1;
    static_cast<void>(::setfsuid(static_cast<uid_t>(previousUid)));
    static_cast<void>(::setfsgid(static_cast<gid_t>(previousGid)));
    if (::setfsuid(static_cast<uid_t>(-1)) != previousUid || ::setfsgid(static_cast<gid_t>(-1)) != previousGid)
    {
        if (file >= 0)
        {
            ::close(file);
        }
        std::fputs("TaskSmack: couldn't restore its filesystem uid; not starting\n", stderr);
        std::_Exit(1);
    }
    return file;
}

std::optional<std::string> readOwnedFile(int fd, uid_t owner, long long mostBytes)
{
    struct stat info = {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != owner || info.st_size > mostBytes)
    {
        return std::nullopt;
    }
    std::string contents;
    std::array<char, 4096> chunk{};
    for (auto got = ::read(fd, chunk.data(), chunk.size()); got > 0; got = ::read(fd, chunk.data(), chunk.size()))
    {
        contents.append(chunk.data(), static_cast<std::size_t>(got));
        if (std::cmp_greater(contents.size(), mostBytes))
        {
            return std::nullopt; // it grew while being read
        }
    }
    return contents;
}

bool isPrivateFolderOf(const std::string& folder, uid_t owner)
{
    struct stat made = {};
    return ::lstat(folder.c_str(), &made) == 0 && S_ISDIR(made.st_mode) && made.st_uid == owner &&
           (made.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

std::map<std::string, std::optional<std::string>, std::less<>>
userSideValuesFrom(const std::function<std::optional<std::string>(std::string_view)>& env)
{
    std::map<std::string, std::optional<std::string>, std::less<>> values;
    for (const std::string_view name : USER_SIDE_NAMES)
    {
        std::optional<std::string> value = env(name);
        // Only XAUTHORITY is recorded unset (#4259): its process value is root's private copy. An unset PATH or
        // XDG_DATA_DIRS isn't recorded, so the child gets the process's own, root's secure path, as before (the #4286 review).
        if (value.has_value() || name == "XAUTHORITY")
        {
            values.emplace(name, std::move(value));
        }
    }
    return values;
}

bool isKept(std::string_view name) noexcept
{
    return std::ranges::find(KEPT, name) != KEPT.end() ||
           std::ranges::any_of(KEPT_PREFIXES, [name](std::string_view prefix) { return name.starts_with(prefix); });
}

bool isLocaleVariable(std::string_view name) noexcept
{
    // sudo's env_check list (LANG, LANGUAGE, LC_*, TERM, COLORTERM), and TaskSmack's own locale override.
    return name == "LANG" || name == "LANGUAGE" || name.starts_with("LC_") || name == "TASKSMACK_LOCALE" || name == "TERM" ||
           name == "COLORTERM";
}

bool isSafeLocale(std::string_view value) noexcept
{
    // Trimmed first, as LocaleSetup's override reader trims it (the #4293 review): " .." is "..".
    std::string_view trimmed = value;
    while (!trimmed.empty() && std::string_view(" \t\n\r\f\v").contains(trimmed.front()))
    {
        trimmed.remove_prefix(1);
    }
    return !value.contains('/') && !value.contains('%') && !trimmed.starts_with('.');
}

bool isSafeTimeZone(std::string_view value) noexcept
{
    if (value.starts_with(':'))
    {
        value.remove_prefix(1);
    }
    if (value.contains(".."))
    {
        return false;
    }
    // A relative name is looked up under the zoneinfo folder; an absolute one must be in it.
    return !value.starts_with('/') || value.starts_with("/usr/share/zoneinfo/");
}

std::vector<std::string> applyTo(const char* const* environment, const RootValues& root, const Operations& ops)
{
    std::map<std::string, std::string, std::less<>> values;
    std::vector<std::string> names;
    if (environment != nullptr)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) - environ is a null-terminated array, as POSIX gives it
        for (const char* const* entry = environment; *entry != nullptr; ++entry)
        {
            const std::string_view text(*entry);
            const std::size_t equals = text.find('=');
            const std::string_view name = text.substr(0, equals);
            const std::string_view value = equals == std::string_view::npos ? std::string_view{} : text.substr(equals + 1);
            if (name.empty())
            {
                continue;
            }
            if (!isKept(name) || (name == "TZ" && !isSafeTimeZone(value)) || (isLocaleVariable(name) && !isSafeLocale(value)))
            {
                names.emplace_back(name);
            }
            else
            {
                values.emplace(name, value);
            }
        }
    }
    std::vector<std::string> removed;
    for (std::string& name : names)
    {
        if (ops.unset(name))
        {
            removed.push_back(std::move(name));
        }
    }
    // Set, or, when that fails, unset: nothing of the user's is left by a failure (the #4187 review).
    // Unset first (every copy: a crafted environ can hold a name twice, and setenv replaces only the first), then set.
    const auto setOrUnset = [&ops](const std::string& name, const std::optional<std::string>& value)
    {
        static_cast<void>(ops.unset(name));
        if (value.has_value() && !ops.set(name, *value))
        {
            static_cast<void>(ops.unset(name));
        }
    };
    // The window still finds the user's Wayland socket once XDG_RUNTIME_DIR is root's: by its absolute path.
    if (const auto display = values.find("WAYLAND_DISPLAY"), runtime = values.find("XDG_RUNTIME_DIR");
        display != values.end() && runtime != values.end() && !display->second.empty() && !display->second.starts_with('/'))
    {
        setOrUnset("WAYLAND_DISPLAY", runtime->second + "/" + display->second);
    }
    setOrUnset("XDG_RUNTIME_DIR", root.runtimeDir);
    setOrUnset("XDG_DATA_DIRS", std::string(ROOT_DATA_DIRS));
    // Never a file the user names: root's own copy of the invoking user's (applyToThisProcess()), or none (the second
    // #4187 review: a user-named Xauthority sent another account's cookie to a display the user chose).
    setOrUnset("XAUTHORITY", values.contains("DISPLAY") ? root.xauthority : std::nullopt);
    setOrUnset("PATH", std::string(ROOT_PATH));
    setOrUnset("HOME", root.home);
    return removed;
}

void applyToThisProcess(bool apply)
{
    if (!apply)
    {
        return;
    }
    RootValues root{.home = rootPasswdHome(), .runtimeDir = std::nullopt, .xauthority = std::nullopt};
    // root's own runtime folder, only if it's root's and no one else's to write.
    if (struct stat runtime = {}; ::lstat("/run/user/0", &runtime) == 0 && S_ISDIR(runtime.st_mode) && runtime.st_uid == 0 &&
                                  (runtime.st_mode & (S_IRWXG | S_IRWXO)) == 0)
    {
        root.runtimeDir = "/run/user/0";
    }
    // The user's own values, for the programs started as them (SessionUser::userSideValues()), an unset one too (#4259).
    SessionUser::userSideValues() = userSideValuesFrom(&SessionUser::processVariable);
    root.xauthority = rootXauthorityCopy(root.runtimeDir);
    removedNames() = applyTo(
        environ,
        root,
        Operations{
            // NOLINTNEXTLINE(concurrency-mt-unsafe,misc-include-cleaner) - first thing in main(), before any thread; unsetenv is in <cstdlib>
            .unset = [](const std::string& name) { return ::unsetenv(name.c_str()) == 0; },
            // NOLINTNEXTLINE(concurrency-mt-unsafe,misc-include-cleaner) - as above; setenv is in <cstdlib>
            .set = [](const std::string& name, const std::string& value) { return ::setenv(name.c_str(), value.c_str(), 1) == 0; },
        });
    // Fail closed: a PATH that isn't root's would be the user's (#4159).
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - as above
    if (const char* path = std::getenv("PATH"); path == nullptr || std::string_view(path) != ROOT_PATH)
    {
        std::fputs("TaskSmack: couldn't set root's PATH in an environment that may be another user's; not starting\n", stderr);
        std::_Exit(1);
    }
}

const std::vector<std::string>& removedFromThisProcess() noexcept
{
    return removedNames();
}

} // namespace Platform::RootEnvironment
