#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#ifdef __linux__
#include "Platform/Linux/DetachedSpawn.h"

#include <functional>
#include <optional>
#endif

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
/// On Linux, runs xdg-open (found on PATH) through Platform::DetachedSpawn (#2138): detached, with
/// none of TaskSmack's descriptors and stdio on /dev/null. Success means xdg-open was exec'd; a missing
/// xdg-open, or one that can't be exec'd, is a failure. Whether xdg-open then finds a handler isn't known.
/// On Windows, uses ShellExecuteW with UTF-8 to UTF-16 conversion.
/// Returns true on success, false on failure (with a logged warning).
/// Can block (a shell extension, COM, a network folder, a fork and exec): call it from a worker, as
/// App::BackgroundOpen does, never from the UI thread (#3155).
[[nodiscard]] bool openWithSystemHandler(std::string_view target);

/// Open a filesystem path with the system's default handler.
/// Handles UTF-16 conversion on Windows using the native path representation,
/// avoiding any ANSI/UTF-8 encoding issues with non-ASCII paths.
/// Returns true on success, false on failure (with a logged warning). Can block, as the other does.
[[nodiscard]] bool openWithSystemHandler(const std::filesystem::path& path);

#ifdef __linux__
namespace Detail
{
/// Who to open as: SessionUser::invokingUserSession() in the app, a stand-in in tests (#2461).
using SessionLookup = std::function<std::optional<Platform::DetachedSpawn::RunAs>()>;

/// openWithSystemHandler() for a UTF-8 target, opening as @p session's user when it names one.
[[nodiscard]] bool openWithSystemHandler(std::string_view target, const SessionLookup& session);
} // namespace Detail
#endif

} // namespace App::PlatformOpen
