#pragma once

#include <filesystem>
#include <string_view>

namespace App::PlatformOpen
{

/// Open a URL or UTF-8 encoded string with the system's default handler.
/// On Linux, uses xdg-open via a double-fork to avoid zombie processes.
/// On Windows, uses ShellExecuteW with UTF-8 to UTF-16 conversion.
/// Returns true on success, false on failure (with a logged warning).
[[nodiscard]] bool openWithSystemHandler(std::string_view target);

/// Open a filesystem path with the system's default handler.
/// Handles UTF-16 conversion on Windows using the native path representation,
/// avoiding any ANSI/UTF-8 encoding issues with non-ASCII paths.
/// Returns true on success, false on failure (with a logged warning).
[[nodiscard]] bool openWithSystemHandler(const std::filesystem::path& path);

} // namespace App::PlatformOpen
