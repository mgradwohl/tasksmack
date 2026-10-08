#include "SelectOverride.h"

#ifdef _WIN32
#include "Platform/Windows/WinString.h"
#endif

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <climits>
#include <cstddef>

#include <windows.h>
#endif

namespace App::SelectOverride
{

#ifdef _WIN32
namespace
{

/// Ordinal, case-insensitive, on UTF-16: how Windows compares file names. Text that isn't valid
/// UTF-8 (or too long for the API) falls back to an exact comparison.
bool equalsOrdinalIgnoreCase(const std::string_view a, const std::string_view b)
{
    const std::wstring wideA = Platform::WinString::utf8ToWide(a);
    const std::wstring wideB = Platform::WinString::utf8ToWide(b);
    if ((wideA.empty() && !a.empty()) || (wideB.empty() && !b.empty()) || wideA.size() > static_cast<std::size_t>(INT_MAX) ||
        wideB.size() > static_cast<std::size_t>(INT_MAX))
    {
        return a == b;
    }
    return CompareStringOrdinal(wideA.data(), static_cast<int>(wideA.size()), wideB.data(), static_cast<int>(wideB.size()), TRUE) ==
           CSTR_EQUAL;
}

} // namespace
#endif

bool processNamesEqual(const std::string_view a, const std::string_view b)
{
#ifdef _WIN32
    return equalsOrdinalIgnoreCase(a, b);
#else
    return a == b;
#endif
}

bool tabNamesEqual(const std::string_view a, const std::string_view b)
{
#ifdef _WIN32
    return equalsOrdinalIgnoreCase(a, b);
#else
    return Detail::equalsIgnoreCase(a, b);
#endif
}

const std::optional<Target>& active()
{
    // Read once: a function-local static is initialised exactly once, thread-safely.
    static const std::optional<Target> target = [] -> std::optional<Target>
    {
        const std::string pidName(PID_ENV_VAR);
        const std::string nameName(NAME_ENV_VAR);
        const std::string tabName(TAB_ENV_VAR);
        ParseResult parsed = parse(SDL_getenv(pidName.c_str()), SDL_getenv(nameName.c_str()), SDL_getenv(tabName.c_str()));
        for (const std::string& warning : parsed.warnings)
        {
            spdlog::warn("{}", warning);
        }
        if (parsed.target)
        {
            spdlog::info("Startup selection (test hook) pending: will select {}", describe(*parsed.target));
        }
        return std::move(parsed.target);
    }();
    return target;
}

} // namespace App::SelectOverride
