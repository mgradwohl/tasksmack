#include "ConfigDirOverride.h"

#include "Core/EnvRead.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace Core::ConfigDirOverride
{

namespace
{

/// What active() returns. Read once: a function-local static is initialised exactly once,
/// thread-safely. Only setActiveForTesting() replaces it after that.
std::optional<std::filesystem::path>& current()
{
    static std::optional<std::filesystem::path> dir = [] -> std::optional<std::filesystem::path>
    {
        std::optional<std::filesystem::path> parsed = parse(Core::readEnv(ENV_VAR));
        if (!parsed.has_value())
        {
            return std::nullopt;
        }
        std::error_code ec;
        if (const std::filesystem::path absolute = std::filesystem::absolute(*parsed, ec); !ec)
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

/// Whether current() was handed over by an elevated restart (adoptHandedOver()).
bool& handedOver()
{
    static bool handed = false;
    return handed;
}

} // namespace

const std::optional<std::filesystem::path>& active()
{
    return current();
}

void adoptHandedOver(const std::filesystem::path& dir)
{
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(dir, ec);
    current() = ec ? dir : absolute.lexically_normal();
    handedOver() = true;
}

bool fromTestHook()
{
    return current().has_value() && !handedOver();
}

void setActiveForTesting(std::optional<std::filesystem::path> dir)
{
    current() = std::move(dir);
    handedOver() = false;
}

} // namespace Core::ConfigDirOverride
