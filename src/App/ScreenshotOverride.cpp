#include "ScreenshotOverride.h"

#include "Core/EnvRead.h"

#include <spdlog/spdlog.h>

#include <optional>
#include <utility>

namespace App::ScreenshotOverride
{

const std::optional<Request>& active()
{
    // Read once: a function-local static is initialised exactly once, thread-safely.
    static const std::optional<Request> request = [] -> std::optional<Request>
    {
        ParseResult parsed = parse(Core::readEnv(ENV_VAR));
        if (!parsed.warning.empty())
        {
            spdlog::warn("{}: {}", ENV_VAR, parsed.warning);
        }
        if (parsed.request)
        {
            spdlog::info("{} is set: {}; TaskSmack quits once it is written", ENV_VAR, describe(*parsed.request));
        }
        return std::move(parsed.request);
    }();
    return request;
}

} // namespace App::ScreenshotOverride
