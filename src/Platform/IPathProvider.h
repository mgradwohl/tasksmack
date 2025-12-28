#pragma once

#include <filesystem>

namespace Platform
{

/// Interface for platform-specific path queries.
/// Provides access to system directories in a platform-agnostic way.
class IPathProvider
{
  public:
    virtual ~IPathProvider() = default;

    IPathProvider() = default;
    IPathProvider(const IPathProvider&) = default;
    IPathProvider& operator=(const IPathProvider&) = default;
    IPathProvider(IPathProvider&&) = default;
    IPathProvider& operator=(IPathProvider&&) = default;

    /// Get the directory containing the current executable.
    [[nodiscard]] virtual std::filesystem::path getExecutableDir() const = 0;

    /// Get the user's configuration directory.
    /// Linux: ~/.config/tasksmack or $XDG_CONFIG_HOME/tasksmack
    /// Windows: %APPDATA%/TaskSmack
    [[nodiscard]] virtual std::filesystem::path getUserConfigDir() const = 0;
};

} // namespace Platform
