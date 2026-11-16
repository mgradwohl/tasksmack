#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#ifdef __linux__
#include "Platform/Linux/DetachedSpawn.h"

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

/// How an open went (#3658).
enum class OpenResult : std::uint8_t
{
    Opened, ///< The handler was started
    Failed, ///< It couldn't be (the opener logged why)
    /// TaskSmack runs as administrator, and the signed-in user's shell couldn't open it: not opened at all,
    /// so no browser, editor or Explorer window runs elevated. The caller shows the target instead, for the
    /// user to open (logged too).
    Withheld,
};

/// Opens a URL or UTF-8 string with the system's default handler.
/// On Linux, runs xdg-open (found on PATH) through Platform::DetachedSpawn (#2138): detached, with
/// none of TaskSmack's descriptors and stdio on /dev/null. Opened means xdg-open was exec'd; a missing
/// xdg-open, or one that can't be exec'd, is Failed. Whether xdg-open then finds a handler isn't known.
/// On Windows, uses ShellExecuteW with UTF-8 to UTF-16 conversion, or, when TaskSmack runs elevated,
/// Explorer's IShellDispatch2::ShellExecute (Platform/Windows/UnelevatedShell.h, #3658): never an
/// elevated ShellExecuteW, which would start the browser or editor as administrator. Withheld when that
/// shell couldn't. A failure is logged.
/// Can block (a shell extension, COM, a network folder, a fork and exec): call it from a worker, as
/// App::BackgroundOpen does, never from the UI thread (#3155).
[[nodiscard]] OpenResult open(std::string_view target);

/// open() for a filesystem path, in its native form: on Windows the path's own UTF-16, so a non-ASCII
/// path goes through no ANSI or UTF-8 conversion. Can block, as the other does.
[[nodiscard]] OpenResult open(const std::filesystem::path& path);

/// The line a view shows for a Withheld open of @p target (a URL, or a path in UTF-8).
[[nodiscard]] std::string withheldNotice(std::string_view target);

namespace Detail
{
/// The two ways open() can start a handler (#3658); each returns true when it did, and logs why not.
struct Openers
{
    std::function<bool()> direct;     ///< In this process (ShellExecuteW, xdg-open): the handler runs as TaskSmack does
    std::function<bool()> unelevated; ///< Through the signed-in user's shell (Explorer): the handler runs unelevated
};

/// open()'s choice: unelevated, the direct opener; elevated, only the unelevated one, Withheld when it
/// fails or there is none. Never the direct opener when @p elevated, whatever the target: an elevated
/// browser, editor or Explorer window would hand administrator rights to what it loads.
[[nodiscard]] OpenResult openRouted(bool elevated, const Openers& openers);
} // namespace Detail

#ifdef __linux__
namespace Detail
{
/// Who to open as: SessionUser::invokingUserSession() in the app, a stand-in in tests (#2461).
using SessionLookup = std::function<std::optional<Platform::DetachedSpawn::RunAs>()>;

/// open() == Opened for a UTF-8 target, opening as @p session's user when it names one.
[[nodiscard]] bool openWithSystemHandler(std::string_view target, const SessionLookup& session);
} // namespace Detail
#endif

} // namespace App::PlatformOpen
