#pragma once

// The pure half of "Trace system calls (strace)" (#182): finding strace and a terminal emulator,
// building the argv that runs one inside the other, and deciding up front whether ptrace will let
// strace attach. Free of system calls -- every input (PATH, $TERMINAL, what is executable,
// ptrace_scope, credentials) is a parameter -- so it is unit-tested on any Linux host
// (test_SyscallTrace.cpp). LinuxProcessActions supplies the real inputs and DetachedSpawn starts the
// result.
//
// Nothing here goes through a shell: the terminal and strace are exec'd with an argv vector, so a
// process name or PID can never be interpreted as shell syntax.

#include "ProcPrivileges.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::SyscallTrace
{

/// How a terminal emulator is told which program to run in its window. They disagree, and passing the
/// wrong one either opens an empty shell or runs nothing.
enum class TerminalSyntax : std::uint8_t
{
    DashE,      ///< `term -e prog args...` (xterm, konsole, alacritty, x-terminal-emulator; the usual default)
    DashDash,   ///< `term -- prog args...` (gnome-terminal, ptyxis)
    DashX,      ///< `term -x prog args...` (xfce4-terminal, mate-terminal: -e there takes one string)
    Positional, ///< `term prog args...` (kitty, foot)
    WezTerm,    ///< `wezterm start -- prog args...`
};

struct KnownTerminal
{
    std::string_view name;
    TerminalSyntax syntax = TerminalSyntax::DashE;
};

/// Terminals tried, in order, when $TERMINAL names none that can be found. x-terminal-emulator comes
/// first: it is Debian/Ubuntu's administrator-chosen default. The rest cover the common desktops.
inline constexpr std::array<KnownTerminal, 11> FALLBACK_TERMINALS{{
    {.name = "x-terminal-emulator", .syntax = TerminalSyntax::DashE},
    {.name = "gnome-terminal", .syntax = TerminalSyntax::DashDash},
    {.name = "ptyxis", .syntax = TerminalSyntax::DashDash},
    {.name = "konsole", .syntax = TerminalSyntax::DashE},
    {.name = "xfce4-terminal", .syntax = TerminalSyntax::DashX},
    {.name = "mate-terminal", .syntax = TerminalSyntax::DashX},
    {.name = "kitty", .syntax = TerminalSyntax::Positional},
    {.name = "alacritty", .syntax = TerminalSyntax::DashE},
    {.name = "foot", .syntax = TerminalSyntax::Positional},
    {.name = "wezterm", .syntax = TerminalSyntax::WezTerm},
    {.name = "xterm", .syntax = TerminalSyntax::DashE},
}};

/// The final component of @p path ("/usr/bin/kitty" -> "kitty").
[[nodiscard]] constexpr std::string_view baseName(std::string_view path) noexcept
{
    const std::size_t slash = path.rfind('/');
    if (slash != std::string_view::npos)
    {
        path.remove_prefix(slash + 1);
    }
    return path;
}

/// The syntax for a terminal named (or at a path ending in) @p nameOrPath: a known terminal's own,
/// else `-e`, which most terminals and the x-terminal-emulator convention accept.
[[nodiscard]] constexpr TerminalSyntax terminalSyntaxFor(std::string_view nameOrPath) noexcept
{
    const std::string_view name = baseName(nameOrPath);
    for (const KnownTerminal& known : FALLBACK_TERMINALS)
    {
        if (known.name == name)
        {
            return known.syntax;
        }
    }
    return TerminalSyntax::DashE;
}

/// A terminal emulator found to run the tracer in.
struct Terminal
{
    std::string path; ///< Absolute path, exec'd directly.
    TerminalSyntax syntax = TerminalSyntax::DashE;

    friend bool operator==(const Terminal&, const Terminal&) = default;
};

/// What LinuxProcessActions found when it was constructed: strace and a terminal, either of which may be
/// missing (empty path / nullopt), in which case the action is reported unavailable.
struct Tools
{
    std::string tracerPath; ///< Absolute path to strace, or empty.
    std::optional<Terminal> terminal;
};

/// Where @p name would be run from: @p name itself when it contains a '/', otherwise the first
/// directory of @p pathEnv (colon-separated) holding it, as `execvp` would search. Empty and relative
/// PATH entries (".", "bin") are skipped: they would make what runs depend on TaskSmack's working
/// directory. @p isExecutable(path) says whether a candidate is an executable file.
///
/// @return The path to run, or nullopt when there is none.
template<typename IsExecutable>
[[nodiscard]] std::optional<std::string> findExecutable(std::string_view name, std::string_view pathEnv, const IsExecutable& isExecutable)
{
    if (name.empty())
    {
        return std::nullopt;
    }
    if (name.contains('/'))
    {
        // A relative path with a slash ("bin/term") would also depend on the working directory.
        if (!name.starts_with('/'))
        {
            return std::nullopt;
        }
        std::string candidate(name);
        if (isExecutable(candidate))
        {
            return candidate;
        }
        return std::nullopt;
    }
    std::size_t start = 0;
    while (start <= pathEnv.size())
    {
        std::size_t end = pathEnv.find(':', start);
        if (end == std::string_view::npos)
        {
            end = pathEnv.size();
        }
        std::string_view dir = pathEnv.substr(start, end - start);
        if (dir.starts_with('/'))
        {
            while (dir.size() > 1 && dir.ends_with('/'))
            {
                dir.remove_suffix(1);
            }
            std::string candidate(dir);
            if (!candidate.ends_with('/'))
            {
                candidate += '/';
            }
            candidate += name;
            if (isExecutable(candidate))
            {
                return candidate;
            }
        }
        start = end + 1;
    }
    return std::nullopt;
}

/// The terminal to run strace in: the one $TERMINAL names when it can be found (@p envTerminal, a name
/// or an absolute path; empty when unset), else the first of FALLBACK_TERMINALS that can.
/// @p find(name) resolves a name to an executable path, or nullopt (findExecutable() over PATH).
///
/// $TERMINAL is taken as one program name, never split into words or handed to a shell.
template<typename Find> [[nodiscard]] std::optional<Terminal> selectTerminal(std::string_view envTerminal, const Find& find)
{
    if (!envTerminal.empty())
    {
        if (std::optional<std::string> path = find(envTerminal))
        {
            return Terminal{.path = std::move(*path), .syntax = terminalSyntaxFor(envTerminal)};
        }
    }
    for (const KnownTerminal& known : FALLBACK_TERMINALS)
    {
        if (std::optional<std::string> path = find(known.name))
        {
            return Terminal{.path = std::move(*path), .syntax = known.syntax};
        }
    }
    return std::nullopt;
}

/// The strace command line for @p pid: follow every thread (-f; with -p it attaches to all of the
/// process's threads, not just the main one) and timestamp each call (-tt).
[[nodiscard]] inline std::vector<std::string> tracerCommand(const std::string& tracerPath, std::int32_t pid)
{
    return {tracerPath, "-f", "-tt", "-p", std::to_string(pid)};
}

/// The argv that opens @p terminal running @p command. argv[0] is the terminal's path, which is also
/// what is exec'd. Every element is passed to execve() as is: none is ever parsed by a shell.
[[nodiscard]] inline std::vector<std::string> buildTerminalArgv(const Terminal& terminal, std::span<const std::string> command)
{
    std::vector<std::string> argv;
    argv.reserve(command.size() + 3);
    argv.push_back(terminal.path);
    switch (terminal.syntax)
    {
    case TerminalSyntax::DashE:
        argv.emplace_back("-e");
        break;
    case TerminalSyntax::DashDash:
        argv.emplace_back("--");
        break;
    case TerminalSyntax::DashX:
        argv.emplace_back("-x");
        break;
    case TerminalSyntax::Positional:
        break;
    case TerminalSyntax::WezTerm:
        argv.emplace_back("start");
        argv.emplace_back("--");
        break;
    }
    argv.insert(argv.end(), command.begin(), command.end());
    return argv;
}

/// The value of /proc/sys/kernel/yama/ptrace_scope (0-3), or nullopt when unreadable or malformed.
[[nodiscard]] inline std::optional<int> parsePtraceScope(std::string_view text) noexcept
{
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }
    int value = -1;
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || value < 0 || value > 3)
    {
        return std::nullopt;
    }
    return value;
}

/// The real, effective and saved IDs from a /proc/[pid]/status line such as
/// "Uid:\t1000\t1000\t1000\t1000" (@p key "Uid:" or "Gid:"); the fourth, filesystem, ID is not needed.
[[nodiscard]] inline std::optional<std::array<std::uint32_t, 3>> parseStatusIds(std::string_view status, std::string_view key) noexcept
{
    std::size_t lineStart = 0;
    while (lineStart < status.size())
    {
        std::size_t lineEnd = status.find('\n', lineStart);
        if (lineEnd == std::string_view::npos)
        {
            lineEnd = status.size();
        }
        std::string_view line(status.data() + lineStart, lineEnd - lineStart);
        if (line.starts_with(key))
        {
            line.remove_prefix(key.size());
            std::array<std::uint32_t, 3> ids{};
            for (std::uint32_t& id : ids)
            {
                const std::size_t valueStart = line.find_first_not_of(" \t");
                if (valueStart == std::string_view::npos)
                {
                    return std::nullopt;
                }
                line.remove_prefix(valueStart);
                const auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), id);
                if (ec != std::errc{})
                {
                    return std::nullopt;
                }
                line.remove_prefix(static_cast<std::size_t>(ptr - line.data()));
            }
            return ids;
        }
        lineStart = lineEnd + 1;
    }
    return std::nullopt;
}

/// Whether a `security.capability` extended attribute (struct vfs_cap_data, little-endian) gives the
/// file CAP_SYS_PTRACE in its permitted set with the effective flag, so a non-capability-aware program
/// like strace runs with it.
[[nodiscard]] constexpr bool fileCapsGrantSysPtrace(std::span<const std::uint8_t> xattr) noexcept
{
    // magic_etc, then data[0].permitted (capabilities 0-31), as 32-bit little-endian words.
    constexpr std::size_t MIN_SIZE = 8;
    if (xattr.size() < MIN_SIZE)
    {
        return false;
    }
    const auto word = [&xattr](std::size_t offset) -> std::uint32_t
    {
        return static_cast<std::uint32_t>(xattr[offset]) | (static_cast<std::uint32_t>(xattr[offset + 1]) << 8U) |
               (static_cast<std::uint32_t>(xattr[offset + 2]) << 16U) | (static_cast<std::uint32_t>(xattr[offset + 3]) << 24U);
    };
    constexpr std::uint32_t VFS_CAP_FLAGS_EFFECTIVE = 0x000001U;
    const std::uint32_t magicEtc = word(0);
    const std::uint32_t permittedLow = word(4);
    return (magicEtc & VFS_CAP_FLAGS_EFFECTIVE) != 0 && ((permittedLow >> ProcPrivileges::CAP_SYS_PTRACE_BIT) & 1U) != 0;
}

/// What ptrace(2) will be checked against when strace attaches.
struct PtraceContext
{
    std::optional<int> ptraceScope; ///< Yama's kernel.yama.ptrace_scope; nullopt without Yama.
    bool sameCredentials = false;   ///< The target's real/effective/saved UIDs and GIDs are ours, and it is dumpable.
    bool privileged = false;        ///< strace will run with CAP_SYS_PTRACE (we are root, or strace has the file capability).
};

/// Why strace could not attach to @p pid, or nullopt when ptrace should allow it.
///
/// Checked before opening a terminal, because a refused strace exits at once and its window closes
/// before the message can be read. TaskSmack never elevates: the messages say what would allow it.
/// strace runs in a terminal TaskSmack starts, so it is never the target's ancestor; that is why
/// Yama's mode 1 ("descendants only") refuses it as surely as mode 2 does.
[[nodiscard]] inline std::optional<std::string> ptraceRefusal(std::int32_t pid, const PtraceContext& context)
{
    if (context.ptraceScope == 3)
    {
        return std::format("Attaching with ptrace is disabled on this system (/proc/sys/kernel/yama/ptrace_scope is 3, "
                           "which only a reboot undoes), so strace cannot trace process {}",
                           pid);
    }
    if (context.privileged)
    {
        return std::nullopt;
    }
    if (!context.sameCredentials)
    {
        return std::format("Process {} runs as another user (or changed its user or group IDs); tracing it needs root or "
                           "CAP_SYS_PTRACE",
                           pid);
    }
    if (context.ptraceScope == 2)
    {
        return std::format("Only processes with CAP_SYS_PTRACE may attach with ptrace here (/proc/sys/kernel/yama/ptrace_scope is 2), "
                           "so strace cannot trace process {} without root",
                           pid);
    }
    if (context.ptraceScope == 1)
    {
        return std::format("ptrace is limited to a process's own children here (/proc/sys/kernel/yama/ptrace_scope is 1), so strace "
                           "cannot attach to process {}. Set kernel.yama.ptrace_scope to 0, or run TaskSmack as root (CAP_SYS_PTRACE), "
                           "to trace it",
                           pid);
    }
    return std::nullopt;
}

} // namespace Platform::SyscallTrace
