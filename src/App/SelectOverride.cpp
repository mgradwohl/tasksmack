#include "SelectOverride.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <optional>
#include <string>
#include <utility>

namespace App::SelectOverride
{

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
