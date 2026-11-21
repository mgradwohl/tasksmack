#include "SdlEnvironment.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h> // environ

namespace Platform::SdlEnvironment
{

namespace
{

std::vector<std::string>& removedNames() noexcept
{
    static std::vector<std::string> names;
    return names;
}

std::vector<std::string>& startingEnvironment() noexcept
{
    static std::vector<std::string> environment;
    return environment;
}

} // namespace

bool isSdlVariable(std::string_view name) noexcept
{
    if (!name.starts_with("SDL"))
    {
        return false;
    }
    std::string_view rest = name;
    rest.remove_prefix(3); // "SDL", checked above
    while (!rest.empty() && rest.front() >= '0' && rest.front() <= '9')
    {
        rest.remove_prefix(1);
    }
    return rest.starts_with('_');
}

bool isLoaderVariable(std::string_view name) noexcept
{
    return name.starts_with(FORCE_VENDOR_PREFIX) || std::ranges::find(LOADER_VARIABLES, name) != LOADER_VARIABLES.end();
}

std::vector<std::string> sdlVariableNames(const char* const* environment)
{
    std::vector<std::string> names;
    if (environment == nullptr)
    {
        return names;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) - environ is a null-terminated array, as POSIX gives it
    for (const char* const* entry = environment; *entry != nullptr; ++entry)
    {
        const std::string_view text(*entry);
        const std::string_view name = text.substr(0, text.find('='));
        if (isSdlVariable(name) || isLoaderVariable(name))
        {
            names.emplace_back(name);
        }
    }
    return names;
}

std::vector<std::string> removeSdlVariables(const char* const* environment, const std::function<bool(const std::string&)>& unset)
{
    std::vector<std::string> removed;
    for (std::string& name : sdlVariableNames(environment))
    {
        if (unset(name))
        {
            removed.push_back(std::move(name));
        }
    }
    return removed;
}

bool keepPrivilegedLoadersOut(const std::function<bool(const std::string&, const std::string&)>& set)
{
    bool all = true;
    for (const auto& [name, value] : PRIVILEGED_SESSION_SETTINGS)
    {
        all = set(std::string(name), std::string(value)) && all;
    }
    return all;
}

void removeForThisProcess(bool remove)
{
    if (!remove)
    {
        return;
    }
    // As it started (#4651), for the programs started as this same user with no capabilities: copied before any unsetenv.
    for (char** entry = environ; entry != nullptr && *entry != nullptr;
         ++entry) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - environ is a null-terminated C array
    {
        startingEnvironment().emplace_back(*entry);
    }
    removedNames() = removeSdlVariables(environ,
                                        [](const std::string& name)
                                        {
                                            // NOLINTNEXTLINE(concurrency-mt-unsafe,misc-include-cleaner) - first thing in main(), before any thread; unsetenv is provided by <cstdlib>
                                            return ::unsetenv(name.c_str()) == 0;
                                        });
}

void keepPrivilegedLoadersOutOfThisProcess(bool apply)
{
    if (!apply)
    {
        return;
    }
    // A failed setenv (ENOMEM) would leave GTK or the shader cache allowed: stop rather than run with them (#4254).
    if (!keepPrivilegedLoadersOut(
            [](const std::string& name, const std::string& value)
            {
                // NOLINTNEXTLINE(concurrency-mt-unsafe,misc-include-cleaner) - first thing in main(), before any thread; setenv is in <cstdlib>
                return ::setenv(name.c_str(), value.c_str(), 1) == 0;
            }))
    {
        std::fputs("TaskSmack: couldn't keep GTK and the shader cache out of a session whose environment may be another "
                   "user's; not starting\n",
                   stderr);
        std::_Exit(1);
    }
}

const std::vector<std::string>& removedFromThisProcess() noexcept
{
    return removedNames();
}

const std::vector<std::string>& environmentBeforeScrub() noexcept
{
    return startingEnvironment();
}

} // namespace Platform::SdlEnvironment
