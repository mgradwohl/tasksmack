#include "DemoOverride.h"

#include "Core/EnvRead.h"

#include <spdlog/spdlog.h>

#include <optional>
#include <utility>
#include <vector>

namespace App::DemoOverride
{

const std::optional<std::vector<Step>>& active()
{
    // Read once: a function-local static is initialised exactly once, thread-safely.
    static const std::optional<std::vector<Step>> steps = [] -> std::optional<std::vector<Step>>
    {
        ParseResult parsed = parse(Core::readEnv(ENV_VAR));
        if (!parsed.warning.empty())
        {
            spdlog::warn("{}: {}", ENV_VAR, parsed.warning);
        }
        if (parsed.steps)
        {
            spdlog::info(
                "{} is set: {} step(s) from the moment startup settles: {}", ENV_VAR, parsed.steps->size(), describe(*parsed.steps));
        }
        return std::move(parsed.steps);
    }();
    return steps;
}

} // namespace App::DemoOverride
