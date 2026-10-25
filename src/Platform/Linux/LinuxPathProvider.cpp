#include "LinuxPathProvider.h"

#include "Platform/ConfigDirectory.h"
#include "UserNameLookup.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include <pwd.h>
#include <unistd.h>

namespace Platform
{

namespace
{

/// An environment variable's value; nullopt when unset or empty.
[[nodiscard]] std::optional<std::string> readEnv(const char* name)
{
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe) - read at startup, before any thread
    if (value == nullptr || value[0] == '\0')
    {
        return std::nullopt;
    }
    return std::string(value);
}

/// This user's home directory from the passwd entry, for when $HOME is unset; its buffer grows for a large
/// LDAP/SSSD entry (#2498).
[[nodiscard]] std::optional<std::filesystem::path> passwdHome()
{
    return lookUpHomeDirectory(::getuid(), ::getpwuid_r).transform([](const std::string& home) { return std::filesystem::path(home); });
}

} // namespace

std::filesystem::path LinuxPathProvider::getExecutableDir() const
{
    // Read /proc/self/exe symlink to get executable path
    std::error_code errorCode;
    const auto exePath = std::filesystem::read_symlink("/proc/self/exe", errorCode);
    if (!errorCode)
    {
        return exePath.parent_path();
    }

    // Fallback to current directory if symlink read fails.
    // Use the error_code overload so we never throw.
    // If current_path() also fails, returns an empty path ({}) as a last-resort
    // sentinel. PathService's normalization step handles this degenerate case and
    // logs a warning so the condition is visible at runtime.
    std::error_code cwdEc;
    auto cwd = std::filesystem::current_path(cwdEc);
    if (!cwdEc)
    {
        return cwd;
    }
    return {};
}

std::filesystem::path LinuxPathProvider::getUserConfigDir() const
{
    // The same directory as config.toml (UserConfig), by the same rules (#2438): $XDG_CONFIG_HOME/tasksmack
    // unless it's relative or climbs with "..", else ~/.config/tasksmack from $HOME or the passwd entry.
    // Plain getenv, as UserConfig reads it: with file capabilities (setcap, as the elevation notice
    // suggests) secure_getenv hid both variables and sent the themes to the working directory. The
    // working directory is still the last resort, and an empty path if even that fails (PathService
    // logs it).
    std::error_code cwdEc;
    std::filesystem::path cwd = std::filesystem::current_path(cwdEc);
    if (cwdEc)
    {
        cwd.clear();
    }
    return ConfigDirectory::configDirectoryFrom(readEnv("XDG_CONFIG_HOME"), readEnv("HOME"), passwdHome, cwd).directory;
}

} // namespace Platform
