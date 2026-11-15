#include "ConfigDirOverride.h"

#include "Core/EnvRead.h"
#include "Platform/IHandedOverFolder.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <memory>
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

/// An elevated restart's hand-off (adoptHandedOver()), kept apart from current() (#3780) and checked first, so a
/// hand-off session never runs current()'s initialiser: TASKSMACK_CONFIG_DIR is never read, nor its directory made.
struct HandOff
{
    std::optional<std::filesystem::path> dir;                ///< Set: active() is this
    std::weak_ptr<const Platform::IHandedOverFolder> folder; ///< The held folder, which main.cpp owns
};

HandOff& handOff()
{
    static HandOff handed;
    return handed;
}

/// Whether the config directory is the invoking user's, in a session run as root for them (markLessPrivilegedFolder()).
bool& lessPrivilegedFlag()
{
    static bool lessPrivileged = false;
    return lessPrivileged;
}

} // namespace

const std::optional<std::filesystem::path>& active()
{
    if (handOff().dir.has_value())
    {
        return handOff().dir;
    }
    return current();
}

void adoptHandedOver(const std::filesystem::path& dir, std::weak_ptr<const Platform::IHandedOverFolder> folder)
{
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(dir, ec);
    handOff() = HandOff{.dir = ec ? dir : absolute.lexically_normal(), .folder = std::move(folder)};
}

void markLessPrivilegedFolder()
{
    lessPrivilegedFlag() = true;
}

bool handedOver()
{
    return handOff().dir.has_value() || lessPrivilegedFlag();
}

std::shared_ptr<const Platform::IHandedOverFolder> handedOverFolder()
{
    return handOff().dir.has_value() ? handOff().folder.lock() : nullptr;
}

bool handedOverFolderReleased()
{
    return handOff().dir.has_value() && handOff().folder.expired();
}

bool fromTestHook()
{
    return !handOff().dir.has_value() && current().has_value();
}

void setActiveForTesting(std::optional<std::filesystem::path> dir)
{
    handOff() = HandOff{};
    current() = std::move(dir);
    lessPrivilegedFlag() = false;
}

} // namespace Core::ConfigDirOverride
