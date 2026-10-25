#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

/// The pure parts of LinuxProcessActions::createDump() (#807): the core file's name, the gcore command
/// that writes it, and what is said when it can't. gdb's gcore attaches to the process with ptrace,
/// pauses it while it copies its memory out, and writes an ELF core file that gdb opens later.
namespace Platform::LinuxCoreDump
{

/// How many names dumpFileName() tries in turn when the one before exists already.
inline constexpr int MAX_NAME_ATTEMPTS = 100;

/// How a Linux dump is written, for ProcessDump::liveReason: always from the running process, as there is
/// no snapshot of a process to write from (ProcessActionCapabilities::dumpsFromSnapshot is false, #2241).
inline constexpr const char* LIVE_REASON = "gdb's gcore stops it to copy its memory out";

/// The message for a dump stopped because it was asked to; the partial file has been deleted by then.
inline constexpr const char* CANCELLED_MESSAGE = "Cancelled; the partial dump file was deleted";

/// How long a cancelled dump waits for the killed gdb to let go of the process (#2122) before deciding
/// whether to wake it. gcore runs gdb as its child, so killing gcore's group doesn't wait for gdb's exit,
/// and until it has exited the process is still traced.
inline constexpr int DETACH_WAIT_MS = 2000;

/// The state letter of a process from its /proc/<pid>/stat: the field after comm's closing parenthesis
/// (comm may hold ')' itself, so the last one counts). Nullopt when there is none.
[[nodiscard]] inline std::optional<char> stateFromStat(std::string_view stat)
{
    const std::size_t close = stat.rfind(')');
    if (close == std::string_view::npos || close + 2 >= stat.size())
    {
        return std::nullopt;
    }
    return stat[close + 2];
}

/// Whether @p state is a stop: 'T' (stopped by a signal) or 't' (stopped by a tracer -- what a process
/// gdb still holds shows, and what turns into 'T' once the tracer dies without detaching).
[[nodiscard]] constexpr bool isStoppedState(char state) noexcept
{
    return state == 'T' || state == 't';
}

/// The PID tracing the process, from its /proc/<pid>/status ("TracerPid:"); 0 when nothing is. Nullopt
/// without the line.
[[nodiscard]] inline std::optional<std::int32_t> tracerPidFromStatus(std::string_view status)
{
    constexpr std::string_view KEY = "TracerPid:";
    for (std::size_t at = 0; at < status.size();)
    {
        const std::size_t end = std::min(status.find('\n', at), status.size());
        std::string_view line = status.substr(at, end - at);
        at = end + 1;
        if (!line.starts_with(KEY))
        {
            continue;
        }
        line.remove_prefix(KEY.size());
        line.remove_prefix(std::min(line.find_first_not_of(" \t"), line.size()));
        std::int32_t pid = 0;
        const auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), pid);
        if (ec != std::errc{} || ptr == line.data())
        {
            return std::nullopt;
        }
        return pid;
    }
    return std::nullopt;
}

/// The dump file's name: "bash_1234_2026-10-15_04-30-00.core" -- the process name, the PID and the local
/// time, so dumps of one process sort by when they were taken. @p attempt 2 and up (the name before it
/// existed already) adds " (2)" and so on before ".core". A '/' or control character in the name
/// (comm can hold almost anything) becomes '_', and an empty name reads as "process".
[[nodiscard]] inline std::string dumpFileName(std::string_view processName, std::int32_t pid, const std::tm& local, int attempt)
{
    std::string name;
    name.reserve(processName.size());
    for (const char c : processName)
    {
        const auto byte = static_cast<unsigned char>(c);
        name += (c == '/' || byte < 0x20 || byte == 0x7F) ? '_' : c;
    }
    if (name.empty() || name == "." || name == "..")
    {
        name = "process";
    }
    name += std::format("_{}_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}",
                        pid,
                        local.tm_year + 1900,
                        local.tm_mon + 1,
                        local.tm_mday,
                        local.tm_hour,
                        local.tm_min,
                        local.tm_sec);
    if (attempt >= 2)
    {
        name += std::format(" ({})", attempt);
    }
    name += ".core";
    return name;
}

/// What a core of the process holds at least, in bytes, from its /proc/<pid>/status: its resident
/// anonymous and shared memory plus what is swapped out (RssAnon + RssShmem + VmSwap) -- the memory
/// gcore writes under the default coredump_filter. Nullopt without RssAnon (a kernel thread, or a
/// kernel before 4.5); a missing RssShmem or VmSwap counts as none.
[[nodiscard]] inline std::optional<std::uint64_t> coreBytesFromStatus(std::string_view status)
{
    const auto kib = [status](std::string_view key) -> std::optional<std::uint64_t>
    {
        for (std::size_t at = 0; at < status.size();)
        {
            const std::size_t end = std::min(status.find('\n', at), status.size());
            const std::string_view line = status.substr(at, end - at);
            at = end + 1;
            if (!line.starts_with(key) || line.size() <= key.size() || line[key.size()] != ':')
            {
                continue;
            }
            std::string_view value = line.substr(key.size() + 1);
            value.remove_prefix(std::min(value.find_first_not_of(" \t"), value.size()));
            std::uint64_t kibibytes = 0;
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), kibibytes);
            if (ec != std::errc{} || ptr == value.data())
            {
                return std::nullopt;
            }
            return kibibytes;
        }
        return std::nullopt;
    };
    const std::optional<std::uint64_t> anon = kib("RssAnon");
    if (!anon.has_value())
    {
        return std::nullopt;
    }
    constexpr std::uint64_t KIB = 1024;
    return (*anon + kib("RssShmem").value_or(0) + kib("VmSwap").value_or(0)) * KIB;
}

/// The command that writes the core: `gcore -o <prefix> <pid>`, which writes "<prefix>.<pid>".
[[nodiscard]] inline std::vector<std::string> gcoreArgv(const std::string& gcorePath, const std::string& prefix, std::int32_t pid)
{
    return {gcorePath, "-o", prefix, std::to_string(pid)};
}

/// The program that actually attaches when @p gcorePath runs (#2148): gdb's gcore is a shell script that
/// runs the gdb beside it ("$binary_path/gdb"). A file capability on the script itself is never applied,
/// so CAP_SYS_PTRACE has to be looked for on that gdb.
[[nodiscard]] inline std::string gcoreDebuggerPath(const std::string& gcorePath)
{
    return (std::filesystem::path(gcorePath).parent_path() / "gdb").string();
}

/// The file `gcore -o <prefix> <pid>` writes.
[[nodiscard]] inline std::filesystem::path gcoreOutputFile(const std::filesystem::path& prefix, std::int32_t pid)
{
    return {prefix.string() + "." + std::to_string(pid)};
}

/// The last non-empty line of @p text, trimmed: what gcore said last on stderr, usually why it failed.
[[nodiscard]] inline std::string_view lastLine(std::string_view text)
{
    constexpr std::string_view SPACE = " \t\r\n";
    while (!text.empty())
    {
        const std::size_t end = text.find_last_not_of(SPACE);
        if (end == std::string_view::npos)
        {
            return {};
        }
        text = text.substr(0, end + 1);
        const std::size_t start = text.find_last_of('\n');
        const std::string_view line = text.substr(start == std::string_view::npos ? 0 : start + 1);
        const std::size_t first = line.find_first_not_of(SPACE);
        if (first != std::string_view::npos)
        {
            return line.substr(first);
        }
        text = text.substr(0, start == std::string_view::npos ? 0 : start);
    }
    return {};
}

/// Why gcore wrote no core for @p pid: its exit status and the last line it wrote on stderr, when any.
[[nodiscard]] inline std::string gcoreFailureMessage(std::int32_t pid, int exitStatus, std::string_view stderrText)
{
    const std::string_view said = lastLine(stderrText);
    if (said.empty())
    {
        return std::format("gcore couldn't write a dump of process {} (exit status {})", pid, exitStatus);
    }
    return std::format("gcore couldn't write a dump of process {}: {}", pid, said);
}

} // namespace Platform::LinuxCoreDump
