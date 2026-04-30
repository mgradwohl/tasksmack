#pragma once

#include <filesystem>
#include <string_view>

namespace App::PlatformOpen
{

/// Open a URL or ASCII-safe string with the system's default handler.
/// On Linux, uses xdg-open via a double-fork to avoid zombie processes.
/// On Windows, uses ShellExecuteW (target must be UTF-8 encoded).
/// Returns true on success, false on failure (with a logged warning).
[[nodiscard]] bool openWithSystemHandler(std::string_view target);

/// Open a filesystem path with the system's default handler.
/// Handles UTF-16 conversion on Windows using the native path representation,
/// avoiding any ANSI/UTF-8 encoding issues with non-ASCII paths.
/// Returns true on success, false on failure (with a logged warning).
[[nodiscard]] bool openWithSystemHandler(const std::filesystem::path& path);

} // namespace App::PlatformOpen
