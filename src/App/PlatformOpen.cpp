#include "App/PlatformOpen.h"

#ifdef _WIN32
#include "Platform/ThreadApartment.h"
#include "Platform/Windows/UnelevatedShell.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsElevation.h"
#endif

#include <spdlog/spdlog.h>

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

#ifdef __linux__
#include "Platform/Linux/DetachedSpawn.h"
#include "Platform/Linux/SessionUser.h"
#include "Platform/Linux/SyscallTrace.h"

#include <array>
#include <cstdlib>
#include <optional>

#include <unistd.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <shellapi.h>
#include <windows.h>
#endif

namespace App::PlatformOpen
{

NativeTarget nativeTargetFromUtf8(std::string_view target)
{
#ifdef _WIN32
    return Platform::WinString::utf8ToWide(target);
#else
    return NativeTarget{target};
#endif
}

NativeTarget nativeTargetFromPath(const std::filesystem::path& path)
{
    return path.native();
}

#ifdef __linux__
bool Detail::openWithSystemHandler(std::string_view target, const SessionLookup& session)
{
    // Linux: xdg-open, found on PATH as execvp would and started through Platform's DetachedSpawn (#2138):
    // double fork and setsid, every inherited descriptor close-on-exec (TaskSmack's sampler threads keep
    // /proc and /sys files open, and drivers their device nodes), stdio on /dev/null, signals reset, and
    // an exec failure reported back -- so the browser or file manager it starts holds nothing of ours.
    const std::string targetStr = nativeTargetFromUtf8(target);
    const char* const pathEnv = std::getenv("PATH"); // NOLINT(concurrency-mt-unsafe) - read on an open worker (#3155); nothing sets it
    const std::optional<std::string> xdgOpen =
        Platform::SyscallTrace::findExecutable("xdg-open",
                                               pathEnv != nullptr ? std::string_view(pathEnv) : std::string_view{},
                                               [](const std::string& path) { return ::access(path.c_str(), X_OK) == 0; });
    if (!xdgOpen.has_value())
    {
        spdlog::warn("Can't open {}: xdg-open isn't installed", targetStr);
        return false;
    }
    const std::array<std::string, 2> argv{*xdgOpen, targetStr};
    // Run as root for another user (sudo, pkexec): opened as that user, in their session (#2461).
    const std::optional<Platform::DetachedSpawn::RunAs> user = session();
    const auto spawned =
        user.has_value() ? Platform::DetachedSpawn::spawnDetached(argv, *user) : Platform::DetachedSpawn::spawnDetached(argv);
    if (!spawned.has_value())
    {
        spdlog::warn("{}", Platform::DetachedSpawn::spawnFailureMessage(spawned.error(), *xdgOpen));
        return false;
    }
    return true;
}
#endif

#ifdef _WIN32
namespace
{

/// Whether this TaskSmack runs elevated: asked once, as it can't change while the process runs.
[[nodiscard]] bool runningElevated()
{
    static const bool elevated = Platform::Windows::isProcessElevated();
    return elevated;
}

// LCOV_EXCL_START - each launches the real default handler (a browser, editor or Explorer); no test may (#2039)
/// ShellExecuteW in this process: the handler runs as TaskSmack does. @p shown names the target in the log.
[[nodiscard]] bool shellExecuteHere(const std::wstring& target, const std::string& shown)
{
    // On a worker (#3155), in an STA for the call, as ShellExecuteW asks of its caller.
    const Platform::SingleThreadedApartment apartment;
    // ShellExecuteW returns > 32 on success
    const auto* const shellResult = ::ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - ShellExecuteW returns HINSTANCE which must be compared as int
    const auto shellCode = reinterpret_cast<INT_PTR>(shellResult);
    if (shellCode <= 32)
    {
        spdlog::warn("Failed to open via ShellExecuteW (code {}): {}", shellCode, shown);
        return false;
    }
    return true;
}

/// Explorer's IShellDispatch2::ShellExecute: the handler runs as the signed-in user, unelevated (#3658).
[[nodiscard]] bool shellExecuteUnelevated(const std::wstring& target, const std::string& shown)
{
    const auto opened = Platform::Windows::UnelevatedShell::open(target);
    if (!opened.has_value())
    {
        spdlog::warn("TaskSmack runs as administrator, and Explorer couldn't open {} as the signed-in user: {}", shown, opened.error());
        return false;
    }
    spdlog::info("Opened {} through Explorer, unelevated", shown);
    return true;
}
// LCOV_EXCL_STOP

/// open() for a UTF-16 target, named @p shown in the log.
[[nodiscard]] OpenResult openWide(const std::wstring& target, const std::string& shown)
{
    const OpenResult result = Detail::openRouted(runningElevated(),
                                                 {
                                                     .direct = [&target, &shown] { return shellExecuteHere(target, shown); },
                                                     .unelevated = [&target, &shown] { return shellExecuteUnelevated(target, shown); },
                                                 });
    if (result == OpenResult::Withheld)
    {
        spdlog::warn("Not opening {} as administrator: the user is shown it to open instead", shown);
    }
    return result;
}

} // namespace
#endif

OpenResult Detail::openRouted(bool elevated, const Openers& openers)
{
    if (!elevated)
    {
        return (openers.direct && openers.direct()) ? OpenResult::Opened : OpenResult::Failed;
    }
    // Elevated: the signed-in user's shell or nothing. No fallback to the direct opener, for a URL, a file or a
    // folder alike: the browser, the editor (Edit Config File) or the Explorer window it would start runs as
    // administrator, with all it loads and all it can start from there.
    return (openers.unelevated && openers.unelevated()) ? OpenResult::Opened : OpenResult::Withheld;
}

std::string withheldNotice(std::string_view target)
{
    return std::format("Not opened, so that it doesn't run as administrator: Windows' shell couldn't open it as you. "
                       "Open it yourself: {}",
                       target);
}

OpenResult open(std::string_view target)
{
#ifdef _WIN32
    const std::wstring wideTarget = nativeTargetFromUtf8(target);
    if (wideTarget.empty())
    {
        spdlog::warn("Failed to convert UTF-8 target to UTF-16: {}", std::string{target});
        return OpenResult::Failed;
    }
    return openWide(wideTarget, std::string{target});
#elif defined(__linux__)
    const bool opened = Detail::openWithSystemHandler(target, [] { return Platform::SessionUser::invokingUserSession(); });
    return opened ? OpenResult::Opened : OpenResult::Failed;
#else
    spdlog::warn("openWithSystemHandler: not supported on this platform for target: {}", std::string{target});
    return OpenResult::Failed;
#endif
}

OpenResult open(const std::filesystem::path& path)
{
#ifdef _WIN32
    // The native wide path directly, never a narrowed one (#1648).
    const std::wstring widePath = nativeTargetFromPath(path);
    return openWide(widePath, Platform::WinString::wideToUtf8(widePath));
#elif defined(__linux__)
    // A Linux path is bytes, and xdg-open takes them as they are (valid UTF-8 or not).
    const std::string nativePath = nativeTargetFromPath(path);
    return open(std::string_view{nativePath});
#else
    spdlog::warn("openWithSystemHandler: not supported on this platform for path: {}", nativeTargetFromPath(path));
    return OpenResult::Failed;
#endif
}

bool openWithSystemHandler(std::string_view target)
{
    return open(target) == OpenResult::Opened;
}

bool openWithSystemHandler(const std::filesystem::path& path)
{
    return open(path) == OpenResult::Opened;
}

} // namespace App::PlatformOpen
