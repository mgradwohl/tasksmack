#include "LinuxPathProvider.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace Platform
{

namespace
{

/// Helper for safe, read-only environment variable lookup.
/// secure_getenv is preferred because it ignores environment variables in setuid/setgid programs.
/// Falls back to getenv with NOLINT if secure_getenv is unavailable; this is safe here because
/// we only read environment variables and never call setenv/putenv concurrently.
[[nodiscard]] const char* getEnvSafe(const char* name)
{
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    return secure_getenv(name);
#else
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - fallback when secure_getenv unavailable
    return std::getenv(name);
#endif
}

} // namespace

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
    if (const char* xdg = getEnvSafe("XDG_CONFIG_HOME"))
    {
        if (xdg[0] != '\0')
        {
            return std::filesystem::path(xdg) / "tasksmack";
        }
    }

    // Fall back to ~/.config/tasksmack
    if (const char* home = getEnvSafe("HOME"))
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
