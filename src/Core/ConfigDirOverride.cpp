#include "ConfigDirOverride.h"

#include "Core/EnvRead.h"
#include "Core/Utf8Path.h"
#include "Platform/IHandedOverFolder.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Core::ConfigDirOverride
{

namespace
{

/// Whether TASKSMACK_CONFIG_DIR is ignored, in an elevated session (ignoreTestHook(), #3920).
bool& testHookIgnoredFlag()
{
    static bool ignored = false;
    return ignored;
}

/// What active() returns. Read once: a function-local static is initialised exactly once,
/// thread-safely. Only setActiveForTesting() replaces it after that.
std::optional<std::filesystem::path>& current()
{
    static std::optional<std::filesystem::path> dir = [] -> std::optional<std::filesystem::path>
    {
        // An elevated session takes no directory from its environment, which the less-privileged side may have set:
        // nothing is read from it, and nothing created there (#3920).
        if (testHookIgnoredFlag())
        {
            return std::nullopt;
        }
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
    handOff() = HandOff{.dir = Platform::handedOverFolderPath(dir), .folder = std::move(folder)};
}

void markLessPrivilegedFolder()
{
    lessPrivilegedFlag() = true;
}

void ignoreTestHook()
{
    testHookIgnoredFlag() = true;
}

bool testHookIgnored()
{
    // Reading the variable is harmless; only taking it, and making its directory, is what an elevated session must not.
    return testHookIgnoredFlag() && parse(Core::readEnv(ENV_VAR)).has_value();
}

bool handedOver()
{
    return handOff().dir.has_value() || lessPrivilegedFlag();
}

std::shared_ptr<const Platform::IHandedOverFolder> handedOverFolder()
{
    return handOff().dir.has_value() ? handOff().folder.lock() : nullptr;
}

std::optional<std::expected<std::vector<HandedOverFile>, std::string>> readHandedOverFiles(const std::filesystem::path& folder,
                                                                                           std::string_view extension)
{
    // The hold first, kept for the whole listing and every read; then whether a hand-off let go of it.
    const std::shared_ptr<const Platform::IHandedOverFolder> held = handedOverFolder();
    if (held == nullptr)
    {
        if (handedOverFolderReleased())
        {
            return std::unexpected(std::string("the handed-over folder isn't held open as it was when it was checked"));
        }
        return std::nullopt;
    }
    auto names = held->listFiles(folder);
    if (!names.has_value())
    {
        return std::unexpected(std::move(names.error()));
    }
    std::vector<HandedOverFile> files;
    for (const std::filesystem::path& name : *names)
    {
        if (name.extension() != std::filesystem::path(extension))
        {
            continue;
        }
        auto text = held->readFile(folder / name);
        if (!text.has_value())
        {
            spdlog::warn("Skipping {}: {}", Core::pathToUtf8(held->path() / folder / name), text.error());
            continue;
        }
        if (text->has_value())
        {
            files.push_back(HandedOverFile{.path = held->path() / folder / name, .text = std::move(**text)});
        }
    }
    return files;
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
