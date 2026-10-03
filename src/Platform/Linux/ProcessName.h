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
    if (slash == std::string_view::npos)
    {
        return path;
    }
    // Not substr(): it is specified to throw on an out-of-range position, which cannot happen here
    // (slash < size) but which a noexcept function may not rely on the compiler to see.
    path.remove_prefix(slash + 1);
    return path;
}

/// The n-th NUL-separated argument of a raw /proc/[pid]/cmdline buffer, or empty if there is none.
[[nodiscard]] constexpr std::string_view argument(std::string_view rawCmdline, std::size_t index) noexcept
{
    // Walks the buffer by shrinking the view (remove_prefix/remove_suffix) rather than with
    // substr(), which is specified to throw on an out-of-range position.
    for (std::size_t i = 0; i < index; ++i)
    {
        const std::size_t nul = rawCmdline.find('\0');
        if (nul == std::string_view::npos)
        {
            return {};
        }
        rawCmdline.remove_prefix(nul + 1);
    }
    if (const std::size_t end = rawCmdline.find('\0'); end != std::string_view::npos)
    {
        rawCmdline.remove_suffix(rawCmdline.size() - end);
    }
    return rawCmdline;
}

/// Whether `candidate` is a longer name that `comm` could be the truncation of.
[[nodiscard]] constexpr bool extendsTruncatedName(std::string_view comm, std::string_view candidate) noexcept
{
    return candidate.size() > comm.size() && candidate.starts_with(comm);
}

/// The process's full name, recovered from its command line where the kernel truncated it.
///
/// Only a `comm` of exactly KERNEL_COMM_MAX characters can have been truncated, so anything shorter
/// is returned as it is. For a full-length one:
///
///   1. If the base name of argv[0] is a longer spelling of `comm`, that is the name: an ordinary
///      program whose executable name the kernel cut.
///   2. If the base name of argv[0] *is* `comm`, the name is complete -- it merely happens to be 15
///      characters long -- and nothing else is consulted. Without this, a program run with an
///      argument that begins with its own name ("exactly15chars- exactly15chars-option") would be
///      renamed to its argument.
///   3. Otherwise argv[0] is something unrelated to `comm`, which is what a script run through an
///      interpreter looks like: argv[0] is "python3" or "sh", and the kernel took `comm` from the
///      script's file name. Then, and only then, the base name of argv[1] is tried.
///
/// A candidate is accepted only if it *starts with* `comm`. A process that renamed itself with
/// prctl(PR_SET_NAME), a kernel thread (no command line), and a program that overwrote its own
/// argv ("nginx: master process ...") all fail that test and keep their `comm`, so the name is only
/// ever replaced by a longer spelling of the same thing.
///
/// One case can still be wrong: a process that chose a 15-character name for itself, whose argv[0]
/// is unrelated, and whose first argument happens to begin with that name. Telling that apart from
/// a script would need knowing which programs are interpreters, which is not knowable from here.
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

    const std::string_view executable = baseName(argument(rawCmdline, 0));
    if (extendsTruncatedName(comm, executable))
    {
        return executable;
    }
    if (executable == comm)
    {
        return comm;
    }

    const std::string_view script = baseName(argument(rawCmdline, 1));
    return extendsTruncatedName(comm, script) ? script : comm;
}

} // namespace Platform::ProcessName
