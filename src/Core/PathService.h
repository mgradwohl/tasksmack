#pragma once

#include "Platform/IPathProvider.h"

#include <filesystem>
#include <memory>

namespace Core
{

/// Centralized path resolution service owned by Application.
/// Wraps Platform::IPathProvider and caches results so callers
/// never need to include Platform/Factory.h outside of Core.
///
/// Lifetime: Created during Application construction; valid for the
/// lifetime of the Application instance.
class PathService
{
  public:
    PathService();

    PathService(const PathService&) = delete;
    PathService& operator=(const PathService&) = delete;
    PathService(PathService&&) = delete;
    PathService& operator=(PathService&&) = delete;

    ~PathService() = default;

    /// Returns the directory containing the running executable.
    [[nodiscard]] const std::filesystem::path& executableDir() const noexcept;

    /// Returns the user-specific configuration directory.
    /// Linux: ~/.config/tasksmack (or $XDG_CONFIG_HOME/tasksmack)
    /// Windows: %APPDATA%/TaskSmack
    [[nodiscard]] const std::filesystem::path& userConfigDir() const noexcept;

  private:
    std::unique_ptr<Platform::IPathProvider> m_Provider;
    std::filesystem::path m_ExecutableDir;
    std::filesystem::path m_UserConfigDir;
};

} // namespace Core
