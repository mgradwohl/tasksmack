#pragma once

// Recovers a process's full name when the kernel has cut it short (#951).
//
// The name in /proc/[pid]/stat is the kernel's `comm`, which is capped at 15 characters
// (TASK_COMM_LEN is 16 including the terminator). Any longer name is silently truncated at the
// source: "systemd-journald" reads as "systemd-journal", "systemd-resolved" as "systemd-resolve".
// A truncated name that still looks like a plausible name is the worst kind, and no amount of column
// width brings the missing characters back -- `ps -eo comm` shows the same 15.
//
// The full name is usually still available in the command line the probe reads anyway. This is the
// pure decision of whether to trust it, kept apart from the file reading so it is unit-testable.

#include <cstddef>
#include <string_view>

namespace Platform::ProcessName
{

/// Longest name the kernel stores in `comm`. A name of exactly this length may have been cut.
inline constexpr std::size_t KERNEL_COMM_MAX = 15;

/// The part of `path` after its last '/', or all of it if there is none.
[[nodiscard]] constexpr std::string_view baseName(std::string_view path) noexcept
{
    const std::size_t slash = path.rfind('/');
    return (slash == std::string_view::npos) ? path : path.substr(slash + 1);
}

/// The n-th NUL-separated argument of a raw /proc/[pid]/cmdline buffer, or empty if there is none.
[[nodiscard]] constexpr std::string_view argument(std::string_view rawCmdline, std::size_t index) noexcept
{
    std::size_t start = 0;
    for (std::size_t i = 0; i < index; ++i)
    {
        const std::size_t nul = rawCmdline.find('\0', start);
        if (nul == std::string_view::npos)
        {
            return {};
        }
        start = nul + 1;
    }
    if (start >= rawCmdline.size())
    {
        return {};
    }
    const std::size_t end = rawCmdline.find('\0', start);
    return rawCmdline.substr(start, (end == std::string_view::npos) ? std::string_view::npos : end - start);
}

/// Whether `candidate` is a longer name that `comm` could be the truncation of.
[[nodiscard]] constexpr bool extendsTruncatedName(std::string_view comm, std::string_view candidate) noexcept
{
    return candidate.size() > comm.size() && candidate.starts_with(comm);
}

/// The process's full name, recovered from its command line where the kernel truncated it.
///
/// Only a `comm` of exactly KERNEL_COMM_MAX characters can have been truncated, so anything shorter
/// is returned as it is. For a full-length one, two places are tried, in order:
///
///   1. the base name of argv[0] -- the executable, for an ordinary program;
///   2. the base name of argv[1] -- the script, for one run through an interpreter, where argv[0]
///      is "python3" or "sh" and the kernel took `comm` from the script's file name.
///
/// A candidate is accepted only if it *starts with* `comm`. That is what makes this safe: a process
/// that renamed itself with prctl(PR_SET_NAME), a kernel thread (no command line), and a program
/// that overwrote its own argv ("nginx: master process ...") all fail the test and keep their
/// `comm`, so the name is only ever replaced by a longer spelling of the same thing.
///
/// @param comm        Name from /proc/[pid]/stat, already stripped of its parentheses.
/// @param rawCmdline  Contents of /proc/[pid]/cmdline, arguments still NUL-separated.
/// @return A view into `rawCmdline` when the name was recovered, otherwise `comm` itself.
[[nodiscard]] constexpr std::string_view resolveFullName(std::string_view comm, std::string_view rawCmdline) noexcept
{
    if (comm.size() != KERNEL_COMM_MAX)
    {
        return comm;
    }

    for (const std::size_t index : {std::size_t{0}, std::size_t{1}})
    {
        const std::string_view candidate = baseName(argument(rawCmdline, index));
        if (extendsTruncatedName(comm, candidate))
        {
            return candidate;
        }
    }
    return comm;
}

} // namespace Platform::ProcessName
