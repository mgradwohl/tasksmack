#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

/// The pure parts of LinuxProcessActions::createDump() (#807): the core file's name, the gcore command
/// that writes it, and what is said when it can't. gdb's gcore attaches to the process with ptrace,
/// pauses it while it copies its memory out, and writes an ELF core file that gdb opens later.
namespace Platform::LinuxCoreDump
{

/// How many names dumpFileName() tries in turn when the one before exists already.
inline constexpr int MAX_NAME_ATTEMPTS = 100;

/// Why a Linux dump is always of the running process: there is no snapshot of a process to write from.
inline constexpr const char* LIVE_REASON = "Linux has no process snapshots";

/// The message for a dump stopped because it was asked to; the partial file has been deleted by then.
inline constexpr const char* CANCELLED_MESSAGE = "Cancelled; the partial dump file was deleted";

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

/// The command that writes the core: `gcore -o <prefix> <pid>`, which writes "<prefix>.<pid>".
[[nodiscard]] inline std::vector<std::string> gcoreArgv(const std::string& gcorePath, const std::string& prefix, std::int32_t pid)
{
    return {gcorePath, "-o", prefix, std::to_string(pid)};
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
