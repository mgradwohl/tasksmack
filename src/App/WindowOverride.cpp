#include "WindowOverride.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <optional>
#include <string>

namespace App::WindowOverride
{

const std::optional<Geometry>& active()
{
    // Read once: a function-local static is initialised exactly once, thread-safely.
    static const std::optional<Geometry> geometry = []() -> std::optional<Geometry>
    {
        const std::string name(ENV_VAR);
        const ParseResult parsed = parse(SDL_getenv(name.c_str()));
        if (!parsed.warning.empty())
        {
            spdlog::warn("{}: {}", ENV_VAR, parsed.warning);
        }
        if (parsed.geometry)
        {
            // tools/measure-idle.sh records this line to confirm the binary applied the geometry.
            spdlog::info("{} is set: {}; window geometry will not be saved", ENV_VAR, describe(*parsed.geometry));
        }
        return parsed.geometry;
    }();
    return geometry;
}

} // namespace App::WindowOverride
