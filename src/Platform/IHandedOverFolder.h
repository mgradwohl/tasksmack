#pragma once

// The config folder an elevated restart was handed (#2728), held open from the startup check on (#3787): main.cpp
// owns it for the rest of the run, and Core::ConfigDirOverride keeps only a weak reference, so nothing outlives it
// and a hand-off whose folder is no longer held fails closed. On Windows, an elevated session's (Windows::PinnedFolder,
// made by Platform::checkHandedOverConfigDir()); on Linux, a session run as root for a user holds that user's
// (App::LinuxHeldFolder, #4167).
//
// It is also the one way into that folder (#3774): the instance lock, config.toml and its staging file, the user
// themes and their folder are each opened through it, relative to the folder held, never by a path from the drive
// down, and each is checked to be inside it. Names are relative to the folder: "config.toml", "themes/dark.toml".

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform
{

/// @p dir as a handed-over folder's path is kept everywhere (ConfigDirOverride, the held folder): absolute, lexically
/// normal, with no trailing separator, so "C:\Users\pat\cfg\" and "C:\Users\pat\cfg" are the same folder to every
/// comparison of them.
[[nodiscard]] inline std::filesystem::path handedOverFolderPath(const std::filesystem::path& dir)
{
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(dir, ec);
    std::filesystem::path path = (ec ? dir : absolute).lexically_normal();
    if (!path.has_filename() && path.has_relative_path())
    {
        path = path.parent_path(); // "C:\a\cfg\" -> "C:\a\cfg"; a root ("C:\") stays as it is
    }
    return path;
}

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

    /// File @p name in the folder, opened read-write and shared, created if it isn't there: the instance lock's file.
    /// An OS file handle (a Windows HANDLE) the caller closes, or why it couldn't be opened.
    [[nodiscard]] virtual std::expected<void*, std::string> openLockFile(const std::filesystem::path& name) const = 0;

    /// What file @p relative holds; nullopt when it, or a folder on the way to it, isn't there.
    [[nodiscard]] virtual std::expected<std::optional<std::string>, std::string> readFile(const std::filesystem::path& relative) const = 0;

    /// Replaces file @p name in the folder with @p contents: a new file of its own is written beside it, then renamed
    /// over it, so a failure leaves the old one whole.
    [[nodiscard]] virtual std::expected<void, std::string> replaceFile(const std::filesystem::path& name,
                                                                       std::string_view contents) const = 0;

    /// Creates folder @p name in the folder, unless it is already there.
    [[nodiscard]] virtual std::expected<void, std::string> createFolder(const std::filesystem::path& name) const = 0;

    /// The names of the plain files (no folders or links) in folder @p name, sorted; empty when it isn't there.
    [[nodiscard]] virtual std::expected<std::vector<std::filesystem::path>, std::string>
    listFiles(const std::filesystem::path& name) const = 0;

    /// The folder's POSIX descriptor (O_PATH), for a save that keeps a file's owner, mode and access control list
    /// (UserConfig's writeIntoFolderFd(), #4546, #4167); -1 where there is none (Windows).
    [[nodiscard]] virtual int posixFolderFd() const noexcept
    {
        return -1;
    }
};

} // namespace Platform
