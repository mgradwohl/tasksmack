#pragma once

// The XDG runtime directory TaskSmack falls back to when the session didn't provide a usable one
// (Linux only; Core::Application::ensureXdgRuntimeDir() uses it before SDL starts). Pure enough to
// test against temporary directories (#1395): every check is on the paths it is given.

#ifndef _WIN32

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace Core::RuntimeDir
{

/// Whether @p perms give the owner read, write and search, and nobody else anything (the XDG spec's 0700).
[[nodiscard]] inline bool hasSafeOwnerOnlyPermissions(const std::filesystem::perms perms)
{
    // Require that group/other bits are entirely clear (no world/group access)
    // and that the owner has at least read, write, and execute so that the
    // directory is actually usable as an XDG runtime dir.
    constexpr auto forbidden = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    constexpr auto required = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec;
    return ((perms & forbidden) == std::filesystem::perms::none) && ((perms & required) == required);
}

/// Whether @p path is a real directory (not a symlink), owned by this user, with owner-only permissions.
[[nodiscard]] inline bool isUsableRuntimeDir(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto symlink = std::filesystem::symlink_status(path, ec);
    if (ec)
    {
        return false;
    }
    if (std::filesystem::is_symlink(symlink) || !std::filesystem::is_directory(symlink))
    {
        return false;
    }

    const auto status = std::filesystem::status(path, ec);
    if (ec)
    {
        return false;
    }

    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
    {
        return false;
    }

    return (st.st_uid == getuid()) && hasSafeOwnerOnlyPermissions(status.permissions());
}

/// @p systemdPath when it is usable; else @p fallbackPath, created if missing and made 0700 -- unless it
/// is a symlink or not a directory, belongs to another user, or changes under us. std::nullopt when
/// neither can be used (each refusal is logged).
[[nodiscard]] inline std::optional<std::string> chooseRuntimeDir(const std::string& systemdPath, const std::string& fallbackPath)
{
    std::error_code ec;

    if (std::filesystem::exists(systemdPath, ec) && !ec && isUsableRuntimeDir(systemdPath))
    {
        return systemdPath;
    }

    // Guard against symlink and non-directory attacks on predictable /tmp path.
    const auto fallbackSymlink = std::filesystem::symlink_status(fallbackPath, ec);
    if (!ec && std::filesystem::exists(fallbackPath, ec))
    {
        if (std::filesystem::is_symlink(fallbackSymlink) || !std::filesystem::is_directory(fallbackSymlink))
        {
            spdlog::warn("Refusing unsafe XDG fallback path '{}': expected a real directory, not symlink/non-directory", fallbackPath);
            return std::nullopt;
        }
    }

    std::filesystem::create_directories(fallbackPath, ec);
    if (ec)
    {
        spdlog::warn("XDG_RUNTIME_DIR not set and could not create fallback '{}': {}", fallbackPath, ec.message());
        return std::nullopt;
    }

    // Guard against applying permissions to a directory we don't own (e.g. on
    // multi-user systems the predictable /tmp path may have been created by a
    // different UID). Chmod on an unowned directory is a side-effect we avoid.
    struct stat stFallback{};
    if (::stat(fallbackPath.c_str(), &stFallback) != 0 || stFallback.st_uid != getuid())
    {
        spdlog::warn("XDG fallback path '{}' is not owned by current user; cannot use it", fallbackPath);
        return std::nullopt;
    }

    // Re-validate with lstat (symlink_status) immediately before chmod to close
    // the TOCTOU window between the ownership check and the permission change.
    // An attacker could swap the directory for a symlink in that window.
    const auto preChmodStatus = std::filesystem::symlink_status(fallbackPath, ec);
    if (ec || std::filesystem::is_symlink(preChmodStatus) || !std::filesystem::is_directory(preChmodStatus))
    {
        spdlog::warn("XDG fallback path '{}' changed to a symlink or non-directory before chmod; aborting", fallbackPath);
        return std::nullopt;
    }

    std::filesystem::permissions(fallbackPath, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
    if (ec)
    {
        spdlog::warn("Could not set permissions on '{}': {}", fallbackPath, ec.message());
        return std::nullopt;
    }

    if (!isUsableRuntimeDir(fallbackPath))
    {
        spdlog::warn("XDG fallback path '{}' is not usable after creation", fallbackPath);
        return std::nullopt;
    }

    return fallbackPath;
}

/// A connection to the Wayland socket a rejected XDG_RUNTIME_DIR holds, for WAYLAND_SOCKET (#4709, #4810). WSLg without
/// systemd sets XDG_RUNTIME_DIR to /mnt/wslg/runtime-dir, which is 0777 and so refused for TaskSmack's own files, though
/// its wayland-0 is the session's display. Another account can rewrite that folder, so the socket is never reached by
/// its path after a check: @p dir/@p display is opened O_PATH | O_NOFOLLOW (never through a link) and checked on that
/// descriptor (a socket @p uid owns), then connected to through /proc/self/fd/<n>, which names that very inode. One
/// swapped in after the check can't be the one connected to. libwayland takes the connected descriptor from
/// WAYLAND_SOCKET before it looks at WAYLAND_DISPLAY. nullopt when @p display isn't a plain socket name (empty, or with
/// a '/'), or the entry isn't there, isn't a socket, is another account's, or doesn't answer.
[[nodiscard]] inline std::optional<int> connectWaylandSocketIn(const std::filesystem::path& dir, std::string_view display, uid_t uid)
{
    if (display.empty() || display.contains('/'))
    {
        return std::nullopt;
    }
    const std::filesystem::path entry = dir / std::string(display);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const int pathFd = ::open(entry.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (pathFd < 0)
    {
        return std::nullopt;
    }
    struct stat st{};
    const bool owned = ::fstat(pathFd, &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == uid;
    std::optional<int> connected;
    if (owned)
    {
        const std::string viaDescriptor = "/proc/self/fd/" + std::to_string(pathFd);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        static_assert(sizeof(address.sun_path) > 32, "room for /proc/self/fd/<n>");
        std::ranges::copy(viaDescriptor, std::begin(address.sun_path));
        const int socketFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); // libwayland keeps it CLOEXEC too
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - the sockets API takes a generic sockaddr
        if (socketFd >= 0 && ::connect(socketFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0)
        {
            connected = socketFd;
        }
        else if (socketFd >= 0)
        {
            ::close(socketFd);
        }
    }
    ::close(pathFd);
    return connected;
}

} // namespace Core::RuntimeDir

#endif // !_WIN32
