#include "RootEnvironment.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pwd.h>
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

std::vector<std::string> applyTo(const char* const* environment, const std::optional<std::string>& rootHome, const Operations& ops)
{
    std::vector<std::string> names;
    if (environment != nullptr)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) - environ is a null-terminated array, as POSIX gives it
        for (const char* const* entry = environment; *entry != nullptr; ++entry)
        {
            const std::string_view text(*entry);
            const std::string_view name = text.substr(0, text.find('='));
            if (!name.empty() && !isKept(name))
            {
                names.emplace_back(name);
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
    static_cast<void>(ops.set("PATH", std::string(ROOT_PATH)));
    if (rootHome.has_value())
    {
        static_cast<void>(ops.set("HOME", *rootHome));
    }
    else
    {
        static_cast<void>(ops.unset("HOME"));
    }
    return removed;
}

void applyToThisProcess(bool apply)
{
    if (!apply)
    {
        return;
    }
    removedNames() = applyTo(
        environ,
        rootPasswdHome(),
        Operations{
            // NOLINTNEXTLINE(concurrency-mt-unsafe,misc-include-cleaner) - first thing in main(), before any thread; unsetenv is in <cstdlib>
            .unset = [](const std::string& name) { return ::unsetenv(name.c_str()) == 0; },
            // NOLINTNEXTLINE(concurrency-mt-unsafe,misc-include-cleaner) - as above; setenv is in <cstdlib>
            .set = [](const std::string& name, const std::string& value) { return ::setenv(name.c_str(), value.c_str(), 1) == 0; },
        });
}

const std::vector<std::string>& removedFromThisProcess() noexcept
{
    return removedNames();
}

} // namespace Platform::RootEnvironment
