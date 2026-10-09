#include "ConfigDirOverride.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace Core::ConfigDirOverride
{

const std::optional<std::filesystem::path>& active()
{
    // Read once: a function-local static is initialised exactly once, thread-safely.
    static const std::optional<std::filesystem::path> dir = [] -> std::optional<std::filesystem::path>
    {
        std::optional<std::filesystem::path> parsed = parse(SDL_getenv(std::string(ENV_VAR).c_str()));
        if (!parsed.has_value())
        {
            return std::nullopt;
        }
        std::error_code ec;
        if (std::filesystem::path absolute = std::filesystem::absolute(*parsed, ec); !ec)
        {
            parsed = absolute.lexically_normal();
        }
        const auto utf8 = parsed->u8string();
        const std::string display(utf8.begin(), utf8.end());
        ec.clear();
        std::filesystem::create_directories(*parsed, ec);
        if (ec)
        {
            spdlog::warn("{}: could not create {}: {}", ENV_VAR, display, ec.message());
        }
        return parsed;
    }();
    return dir;
}

} // namespace Core::ConfigDirOverride
