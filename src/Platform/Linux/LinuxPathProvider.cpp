#include "LinuxPathProvider.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace Platform
{

std::filesystem::path LinuxPathProvider::getExecutableDir() const
{
    // Read /proc/self/exe symlink to get executable path
    std::error_code errorCode;
    auto exePath = std::filesystem::read_symlink("/proc/self/exe", errorCode);
    if (!errorCode)
    {
        return exePath.parent_path();
    }

    // Fallback to current directory if symlink read fails
    return std::filesystem::current_path();
}

std::filesystem::path LinuxPathProvider::getUserConfigDir() const
{
    // Try XDG_CONFIG_HOME first
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"))
    {
        if (xdg[0] != '\0')
        {
            return std::filesystem::path(xdg) / "tasksmack";
        }
    }

    // Fall back to ~/.config/tasksmack
    if (const char* home = std::getenv("HOME"))
    {
        if (home[0] != '\0')
        {
            return std::filesystem::path(home) / ".config" / "tasksmack";
        }
    }

    // Last resort: current directory
    return std::filesystem::current_path();
}

} // namespace Platform
