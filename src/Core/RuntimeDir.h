#pragma once

// The XDG runtime directory TaskSmack falls back to when the session didn't provide a usable one
// (Linux only; Core::Application::ensureXdgRuntimeDir() uses it before SDL starts). Pure enough to
// test against temporary directories (#1395): every check is on the paths it is given.

#ifndef _WIN32

#include <spdlog/spdlog.h>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include <sys/stat.h>
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

} // namespace Core::RuntimeDir

#endif // !_WIN32
