#pragma once

// Pure helpers behind IProcessEnvironmentReader (#179): parsing a raw environment block into
// NAME=VALUE entries, escaping what a process put there so it is safe to draw, and turning an OS
// error into a read status. Free of OS calls, so they build and are tested on every platform
// (tests/Platform/test_ProcessEnvironment.cpp).

#include "Platform/IProcessEnvironment.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Environment
{

/// The entries of a raw environment block: NUL-separated NAME=VALUE strings, as /proc/[pid]/environ
/// holds them.
///
/// - Empty pieces (an empty block, the trailing NUL, doubled NULs) are skipped.
/// - The name ends at the first '=' after its first character, so a value may itself contain '='
///   and a leading '=' stays in the name (Windows-style "=C:=C:\dir").
/// - An entry without '=' is kept as a name with an empty value rather than dropped.
/// - Both halves are kept as their raw bytes (#4328): the UI makes display text when it draws them
///   (Domain::DisplayText::escaped()).
/// Entries are returned in block order; sorting is up to the caller.
[[nodiscard]] inline std::vector<EnvironmentVariable> parseEnvironBlock(std::string_view raw)
{
    std::vector<EnvironmentVariable> variables;
    while (!raw.empty())
    {
        const std::size_t nul = raw.find('\0');
        const std::string_view entry = raw.substr(0, nul);
        raw.remove_prefix((nul == std::string_view::npos) ? raw.size() : nul + 1);
        if (entry.empty())
        {
            continue;
        }
        const std::size_t equals = entry.find('=', 1);
        const std::string_view name = entry.substr(0, equals);
        const std::string_view value = (equals == std::string_view::npos) ? std::string_view{} : entry.substr(equals + 1);
        variables.push_back({.name = std::string(name), .value = std::string(value)});
    }
    return variables;
}

/// The status a failed environment read reports for the OS error @p err (an errno value):
/// EACCES/EPERM are PermissionDenied (another user's process), ENOENT/ESRCH are ProcessExited (its
/// /proc directory is gone, or the directory handle outlived the process), anything else Failed.
[[nodiscard]] constexpr EnvironmentReadStatus statusFromErrno(int err) noexcept
{
    switch (err)
    {
    case EACCES:
    case EPERM:
        return EnvironmentReadStatus::PermissionDenied;
    case ENOENT:
    case ESRCH:
        return EnvironmentReadStatus::ProcessExited;
    default:
        return EnvironmentReadStatus::Failed;
    }
}

} // namespace Platform::Environment
