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

bool keepGtkOut(const std::function<bool(const std::string&, const std::string&)>& set)
{
    return set(std::string(NO_LIBDECOR_VARIABLE), std::string(NO_LIBDECOR_VALUE));
}

void removeForThisProcess(bool remove)
{
    if (!remove)
    {
        return;
    }
    removedNames() = removeSdlVariables(environ,
                                        [](const std::string& name)
                                        {
                                            // NOLINTNEXTLINE(concurrency-mt-unsafe,misc-include-cleaner) - first thing in main(), before any thread; unsetenv is provided by <cstdlib>
                                            return ::unsetenv(name.c_str()) == 0;
                                        });
    // After the scrub, which removed any SDL variable the user's environment set, this one included. A failed setenv
    // (ENOMEM) leaves libdecor allowed: stop rather than run GTK with this session's privileges (#4254).
    if (!keepGtkOut(
            [](const std::string& name, const std::string& value)
            {
                // NOLINTNEXTLINE(concurrency-mt-unsafe,misc-include-cleaner) - first thing in main(), before any thread; setenv is in <cstdlib>
                return ::setenv(name.c_str(), value.c_str(), 1) == 0;
            }))
    {
        std::fputs("TaskSmack: couldn't keep GTK out of a session whose environment may be another user's; not starting\n", stderr);
        std::_Exit(1);
    }
}

const std::vector<std::string>& removedFromThisProcess() noexcept
{
    return removedNames();
}

} // namespace Platform::SdlEnvironment
