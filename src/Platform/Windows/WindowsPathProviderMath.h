#pragma once

// Pure logic extracted from WindowsPathProvider.cpp so the "OS primitive keeps failing" fallback
// branches - which need a genuinely broken environment to reach for real (GetModuleFileNameW
// failing, APPDATA missing, current_path() failing) - can be exercised by injecting fabricated
// behavior instead. See CONTRIBUTING.md's "extract the pure decision logic into a small header"
// pattern (also used by Core/PathServiceMath.h for the equivalent Core-layer fallback).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>

namespace Platform
{

using PathProviderCurrentPathFn = std::function<std::filesystem::path(std::error_code&)>;

/// Pure retry-loop logic extracted from WindowsPathProvider::getExecutableDir(): grows a wide
/// string buffer and calls @p getModuleFileName repeatedly until the path fits or the Windows
/// long-path limit is hit, then falls back to @p currentPathFn.
/// @param getModuleFileName Mimics GetModuleFileNameW: writes into the given buffer/size and
/// returns the length written (0 = failure, >= size = buffer too small / result truncated).
[[nodiscard]] inline std::filesystem::path
resolveExecutableDir(const std::function<std::uint32_t(wchar_t* buffer, std::uint32_t size)>& getModuleFileName,
                     const PathProviderCurrentPathFn& currentPathFn)
{
    constexpr std::uint32_t kInitialSize = 260;   // MAX_PATH
    constexpr std::uint32_t kMaxLongPath = 32767; // Windows long-path limit for Unicode paths

    std::wstring buffer(kInitialSize, L'\0');

    for (;;)
    {
        const auto bufferSize = static_cast<std::uint32_t>(buffer.size());
        const std::uint32_t len = getModuleFileName(buffer.data(), bufferSize);

        if (len == 0)
        {
            // Failure; break to fallback.
            break;
        }

        if (len < bufferSize)
        {
            // Successfully retrieved full path (len does not include terminating null).
            buffer.resize(len);
            return std::filesystem::path(buffer).parent_path();
        }

        // len >= bufferSize: the buffer may be too small and the path truncated.
        if (bufferSize >= kMaxLongPath)
        {
            // Already at or above the long-path limit; cannot grow further safely.
            break;
        }

        const std::uint32_t newSize = std::min(bufferSize * 2, kMaxLongPath);
        buffer.assign(static_cast<std::size_t>(newSize), L'\0');
    }

    // Fallback to current directory if GetModuleFileName fails. If current_path() also fails,
    // returns an empty path ({}) as a last-resort sentinel; PathService's normalization step
    // handles this degenerate case and logs a warning so the condition is visible at runtime.
    std::error_code cwdEc;
    auto cwd = currentPathFn(cwdEc);
    if (!cwdEc)
    {
        return cwd;
    }
    return {};
}

/// Pure fallback logic extracted from WindowsPathProvider::getUserConfigDir(): if @p appDataEnv
/// returns a non-empty value, appends "TaskSmack" to it; otherwise falls back to @p
/// currentPathFn (same last-resort-empty-path contract as resolveExecutableDir() above).
/// @param appDataEnv Mimics reading the APPDATA environment variable: nullopt or an empty string
/// both count as "not found".
[[nodiscard]] inline std::filesystem::path resolveUserConfigDir(const std::function<std::optional<std::string>()>& appDataEnv,
                                                                const PathProviderCurrentPathFn& currentPathFn)
{
    if (const auto appData = appDataEnv(); appData.has_value() && !appData->empty())
    {
        return std::filesystem::path(*appData) / "TaskSmack";
    }

    std::error_code cwdEc;
    auto cwd = currentPathFn(cwdEc);
    if (!cwdEc)
    {
        return cwd;
    }
    return {};
}

} // namespace Platform
