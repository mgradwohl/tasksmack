#include "App/PlatformOpen.h"

#ifdef _WIN32
#include "Platform/ThreadApartment.h"
#include "Platform/Windows/WinString.h"
#endif

#include <spdlog/spdlog.h>

#include <filesystem>
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

[[nodiscard]] bool openWithSystemHandler(std::string_view target)
{
#ifdef _WIN32
    const std::wstring wideTarget = nativeTargetFromUtf8(target);
    if (wideTarget.empty())
    {
        spdlog::warn("Failed to convert UTF-8 target to UTF-16: {}", std::string{target});
        return false;
    }

    // LCOV_EXCL_START - launches the real default handler (a browser, Explorer); no test may (#2039)
    // On a worker (#3155), in an STA for the call, as ShellExecuteW asks of its caller.
    const Platform::SingleThreadedApartment apartment;
    // ShellExecuteW returns > 32 on success
    const auto* const shellResult = ::ShellExecuteW(nullptr, L"open", wideTarget.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - ShellExecuteW returns HINSTANCE which must be compared as int
    const auto shellCode = reinterpret_cast<INT_PTR>(shellResult);
    if (shellCode <= 32)
    {
        spdlog::warn("Failed to open target via ShellExecuteW (code {}): {}", shellCode, std::string{target});
        return false;
    }
    return true;
    // LCOV_EXCL_STOP
#elif defined(__linux__)
    return Detail::openWithSystemHandler(target, [] { return Platform::SessionUser::invokingUserSession(); });
#else
    spdlog::warn("openWithSystemHandler: not supported on this platform for target: {}", std::string{target});
    return false;
#endif
}

[[nodiscard]] bool openWithSystemHandler(const std::filesystem::path& path)
{
#ifdef _WIN32
    // LCOV_EXCL_START - launches the real default handler (a browser, Explorer); no test may (#2039)
    // The native wide path directly, never a narrowed one (#1648).
    const std::wstring widePath = nativeTargetFromPath(path);

    // On a worker (#3155), in an STA for the call, as ShellExecuteW asks of its caller.
    const Platform::SingleThreadedApartment apartment;
    // ShellExecuteW returns > 32 on success
    const auto* const shellResult = ::ShellExecuteW(nullptr, L"open", widePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - ShellExecuteW returns HINSTANCE which must be compared as int
    const auto shellCode = reinterpret_cast<INT_PTR>(shellResult);
    if (shellCode <= 32)
    {
        spdlog::warn("Failed to open path via ShellExecuteW (code {}): {}", shellCode, Platform::WinString::wideToUtf8(widePath));
        return false;
    }
    return true;
    // LCOV_EXCL_STOP
#elif defined(__linux__)
    // A Linux path is bytes, and xdg-open takes them as they are (valid UTF-8 or not).
    const std::string nativePath = nativeTargetFromPath(path);
    return openWithSystemHandler(std::string_view{nativePath});
#else
    spdlog::warn("openWithSystemHandler: not supported on this platform for path: {}", nativeTargetFromPath(path));
    return false;
#endif
}

} // namespace App::PlatformOpen
