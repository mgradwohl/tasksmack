#pragma once

// Open file location (#3064): what LinuxProcessActions::openFileLocation() asks the file manager for.
// It reads the process's executable from /proc/<pid>/exe, asks the session's file manager to show it
// selected (org.freedesktop.FileManager1.ShowItems), and when none answers opens its folder with
// xdg-open. These are the pure parts, tested on their own.

#include "Platform/UriEncoding.h"

#include <string>
#include <string_view>

namespace Platform::FileManagerReveal
{

/// What the kernel appends to /proc/<pid>/exe's link when the file has been deleted or replaced since
/// the process started (an upgraded package's old binary).
inline constexpr std::string_view DELETED_SUFFIX = " (deleted)";

/// /proc/<pid>/exe's link text without DELETED_SUFFIX: the path the executable had.
[[nodiscard]] constexpr std::string_view withoutDeletedSuffix(std::string_view link) noexcept
{
    if (link.ends_with(DELETED_SUFFIX))
    {
        link.remove_suffix(DELETED_SUFFIX.size());
    }
    return link;
}

/// The file:// URI of absolute path @p path, each byte but the unreserved set and '/' percent-encoded,
/// as ShowItems takes it.
[[nodiscard]] inline std::string fileUri(std::string_view path)
{
    return "file://" + percentEncode(path, "/");
}

/// The folder holding absolute path @p path: what xdg-open opens when no file manager answers ShowItems.
/// "/" for a file at the root.
[[nodiscard]] constexpr std::string_view parentFolder(std::string_view path) noexcept
{
    const std::size_t slash = path.rfind('/');
    if (slash == std::string_view::npos || slash == 0)
    {
        return "/";
    }
    path.remove_suffix(path.size() - slash);
    return path;
}

} // namespace Platform::FileManagerReveal
