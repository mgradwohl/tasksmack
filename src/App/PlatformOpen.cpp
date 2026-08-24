#include "App/PlatformOpen.h"

#ifdef _WIN32
#include "Platform/Windows/WinString.h"
#endif

#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>
#include <string_view>

// NOLINTBEGIN(misc-include-cleaner) - POSIX headers: include-cleaner lacks mappings for pid_t, wait macros
#ifdef __linux__
#include <cerrno>
#include <cstdlib>
#include <system_error>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
// NOLINTEND(misc-include-cleaner)

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <shellapi.h>
#include <windows.h>
#endif

namespace App::PlatformOpen
{

[[nodiscard]] bool openWithSystemHandler(std::string_view target)
{
#ifdef _WIN32
    const std::wstring wideTarget = Platform::WinString::utf8ToWide(target);
    if (wideTarget.empty())
    {
        spdlog::warn("Failed to convert UTF-8 target to UTF-16: {}", std::string{target});
        return false;
    }

    // ShellExecuteW returns > 32 on success
    auto* const shellResult = ::ShellExecuteW(nullptr, L"open", wideTarget.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - ShellExecuteW returns HINSTANCE which must be compared as int
    const auto shellCode = reinterpret_cast<INT_PTR>(shellResult);
    if (shellCode <= 32)
    {
        spdlog::warn("Failed to open target via ShellExecuteW (code {}): {}", shellCode, std::string{target});
        return false;
    }
    return true;
#elif defined(__linux__)
    // Linux: Use double-fork to safely spawn xdg-open without creating zombies
    const std::string targetStr{target};
    // NOLINTNEXTLINE(misc-include-cleaner) - pid_t from sys/types.h, include-cleaner false positive
    const pid_t pid = ::fork();
    if (pid == -1)
    {
        spdlog::warn("Failed to fork process for xdg-open: {}", std::system_category().message(errno));
        return false;
    }

    if (pid == 0)
    {
        // First child: fork again to create grandchild
        const pid_t grandchild = ::fork();
        if (grandchild == -1)
        {
            _exit(EXIT_FAILURE);
        }

        if (grandchild == 0)
        {
            // Grandchild: exec xdg-open (will be adopted by init when first child exits)
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): execlp varargs require a typed null char* terminator
            ::execlp("xdg-open", "xdg-open", targetStr.c_str(), static_cast<char*>(nullptr));
            _exit(127); // execlp only returns on error
        }
        // First child exits immediately (grandchild will be adopted by init)
        _exit(0);
    }

    // Parent: wait for first child to prevent zombie; retry on EINTR
    int status = 0;
    pid_t waited = ::waitpid(pid, &status, 0);
    while (waited == -1 && errno == EINTR)
    {
        waited = ::waitpid(pid, &status, 0);
    }

    if (waited == -1)
    {
        spdlog::warn("waitpid failed for xdg-open launcher: {}", std::system_category().message(errno));
        return false;
    }
    // NOLINTNEXTLINE(misc-include-cleaner) - WIFEXITED/WEXITSTATUS from sys/wait.h, include-cleaner false positive
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
    {
        // NOLINTNEXTLINE(misc-include-cleaner) - WEXITSTATUS from sys/wait.h
        spdlog::warn("xdg-open launcher exited with code {}", WEXITSTATUS(status));
        return false;
    }
    // NOLINTNEXTLINE(misc-include-cleaner) - WIFSIGNALED from sys/wait.h, include-cleaner false positive
    if (WIFSIGNALED(status))
    {
        // NOLINTNEXTLINE(misc-include-cleaner) - WTERMSIG from sys/wait.h
        spdlog::warn("xdg-open launcher killed by signal {}", WTERMSIG(status));
        return false;
    }
    // Launcher reaped successfully. Note: grandchild exec failure (e.g., xdg-open
    // not installed) is not detectable here by design — the grandchild is adopted
    // by init before the parent can observe its exit status.
    return true;
#else
    spdlog::warn("openWithSystemHandler: not supported on this platform for target: {}", std::string{target});
    return false;
#endif
}

[[nodiscard]] bool openWithSystemHandler(const std::filesystem::path& path)
{
#ifdef _WIN32
    // Use the native wide path directly to avoid ANSI/UTF-8 encoding issues.
    const std::wstring widePath = path.wstring();

    // ShellExecuteW returns > 32 on success
    auto* const shellResult = ::ShellExecuteW(nullptr, L"open", widePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - ShellExecuteW returns HINSTANCE which must be compared as int
    const auto shellCode = reinterpret_cast<INT_PTR>(shellResult);
    if (shellCode <= 32)
    {
        spdlog::warn("Failed to open path via ShellExecuteW (code {}): {}", shellCode, Platform::WinString::wideToUtf8(widePath));
        return false;
    }
    return true;
#elif defined(__linux__)
    // On Linux, std::filesystem::path::string() typically returns a UTF-8 string
    // (the encoding is locale-dependent, but virtually all Linux distros use UTF-8).
    return openWithSystemHandler(std::string_view{path.string()});
#else
    spdlog::warn("openWithSystemHandler: not supported on this platform for path: {}", path.string());
    return false;
#endif
}

} // namespace App::PlatformOpen
