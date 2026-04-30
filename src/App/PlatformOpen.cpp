#include "App/PlatformOpen.h"

#include <spdlog/spdlog.h>

#include <string>
#include <string_view>

// NOLINTBEGIN(misc-include-cleaner) - POSIX headers: include-cleaner lacks mappings for pid_t, wait macros
#ifdef __linux__
#include <cerrno>
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
#include <system_error>
#include <windows.h>
#endif

namespace App::PlatformOpen
{

[[nodiscard]] bool openWithSystemHandler(std::string_view target)
{
    const std::string targetStr{target};

#ifdef _WIN32
    // Properly convert UTF-8 to UTF-16 using MultiByteToWideChar
    const int wideSize = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, targetStr.c_str(), -1, nullptr, 0);
    if (wideSize == 0)
    {
        spdlog::warn("Failed to convert UTF-8 target to UTF-16: {}", targetStr);
        return false;
    }

    // wideSize includes the null terminator; std::wstring length excludes it.
    std::wstring wideTarget(static_cast<std::size_t>(wideSize - 1), L'\0');
    const int result = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, targetStr.c_str(), -1, wideTarget.data(), wideSize);
    if (result == 0)
    {
        spdlog::warn("Failed to convert UTF-8 target to UTF-16 on second pass: {}", targetStr);
        return false;
    }

    // ShellExecuteW returns > 32 on success
    auto* const shellResult = ::ShellExecuteW(nullptr, L"open", wideTarget.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - ShellExecuteW returns HINSTANCE which must be compared as int
    const auto shellCode = reinterpret_cast<INT_PTR>(shellResult);
    if (shellCode <= 32)
    {
        spdlog::warn("Failed to open target via ShellExecuteW (code {}): {}", shellCode, targetStr);
        return false;
    }
    return true;
#else
    // Linux: Use double-fork to safely spawn xdg-open without creating zombies
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
            ::execlp("xdg-open", "xdg-open", targetStr.c_str(), nullptr);
            _exit(127); // execlp only returns on error
        }
        // First child exits immediately (grandchild will be adopted by init)
        _exit(0);
    }

    // Parent: wait for first child to prevent zombie
    int status = 0;
    const pid_t waited = ::waitpid(pid, &status, 0);
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
    return true;
#endif
}

} // namespace App::PlatformOpen
