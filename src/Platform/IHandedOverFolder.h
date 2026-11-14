#pragma once

// The config folder an elevated restart was handed (#2728), held open from the startup check on (#3787): main.cpp
// owns it for the rest of the run, and Core::ConfigDirOverride keeps only a weak reference, so nothing outlives it
// and a hand-off whose folder is no longer held fails closed. Made by Platform::checkHandedOverConfigDir(); Windows
// only (Windows::ElevatedRestart::PinnedFolder).

#include <filesystem>

namespace Platform
{

class IHandedOverFolder
{
  public:
    IHandedOverFolder() = default;
    virtual ~IHandedOverFolder() = default;

    IHandedOverFolder(const IHandedOverFolder&) = delete;
    IHandedOverFolder& operator=(const IHandedOverFolder&) = delete;
    IHandedOverFolder(IHandedOverFolder&&) = delete;
    IHandedOverFolder& operator=(IHandedOverFolder&&) = delete;

    /// The folder's path, as it was checked.
    [[nodiscard]] virtual const std::filesystem::path& path() const noexcept = 0;
};

} // namespace Platform
