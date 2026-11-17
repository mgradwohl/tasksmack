#include "RootEnvironment.h"

#include "SessionUser.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pwd.h>
#include <sys/stat.h>
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

} // namespace

bool isKept(std::string_view name) noexcept
{
    return std::ranges::find(KEPT, name) != KEPT.end() ||
           std::ranges::any_of(KEPT_PREFIXES, [name](std::string_view prefix) { return name.starts_with(prefix); });
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
            if (!isKept(name) || (name == "TZ" && !isSafeTimeZone(value)))
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
    const auto setOrUnset = [&ops](const std::string& name, const std::optional<std::string>& value)
    {
        if (!value.has_value() || !ops.set(name, *value))
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
    if (values.contains("DISPLAY") && !values.contains("XAUTHORITY") && root.userXauthority.has_value())
    {
        setOrUnset("XAUTHORITY", root.userXauthority);
    }
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
    RootValues root{.home = rootPasswdHome(), .runtimeDir = std::nullopt, .userXauthority = std::nullopt};
    // root's own runtime folder, only if it's root's and no one else's to write.
    if (struct stat runtime = {}; ::lstat("/run/user/0", &runtime) == 0 && S_ISDIR(runtime.st_mode) && runtime.st_uid == 0 &&
                                  (runtime.st_mode & (S_IRWXG | S_IRWXO)) == 0)
    {
        root.runtimeDir = "/run/user/0";
    }
    // The invoking user's ~/.Xauthority, from their passwd entry, never HOME (the #4187 review).
    if (const std::optional<std::string> userHome = SessionUser::invokingUserHome(); userHome.has_value())
    {
        const std::string candidate = *userHome + "/.Xauthority";
        if (struct stat file = {}; ::stat(candidate.c_str(), &file) == 0 && S_ISREG(file.st_mode))
        {
            root.userXauthority = candidate;
        }
    }
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
