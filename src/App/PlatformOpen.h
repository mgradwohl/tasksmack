#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace App::PlatformOpen
{

/// The argument the system handler is given: UTF-16 for ShellExecuteW on Windows, the bytes
/// xdg-open gets as argv elsewhere.
#ifdef _WIN32
using NativeTarget = std::wstring;
#else
using NativeTarget = std::string;
#endif

/// The handler's argument for a UTF-8 URL or string. On Windows, converted from UTF-8 (never
/// through the code page), and empty when @p target isn't valid UTF-8; elsewhere, the same bytes.
[[nodiscard]] NativeTarget nativeTargetFromUtf8(std::string_view target);

/// The handler's argument for a path: the path's own native form (UTF-16 on Windows, bytes on
/// Linux), so no character is lost to a code page conversion (#1648).
[[nodiscard]] NativeTarget nativeTargetFromPath(const std::filesystem::path& path);

/// Open a URL or UTF-8 encoded string with the system's default handler.
/// On Linux, uses xdg-open via a double-fork to avoid zombie processes.
/// Note: on Linux, success only means the launcher process was forked and
/// reaped cleanly — it cannot detect whether xdg-open itself was found or
/// succeeded (exec failure in the grandchild is silent by design).
/// On Windows, uses ShellExecuteW with UTF-8 to UTF-16 conversion.
/// Returns true on success, false on failure (with a logged warning).
[[nodiscard]] bool openWithSystemHandler(std::string_view target);

/// Open a filesystem path with the system's default handler.
/// Handles UTF-16 conversion on Windows using the native path representation,
/// avoiding any ANSI/UTF-8 encoding issues with non-ASCII paths.
/// Returns true on success, false on failure (with a logged warning).
[[nodiscard]] bool openWithSystemHandler(const std::filesystem::path& path);

} // namespace App::PlatformOpen
