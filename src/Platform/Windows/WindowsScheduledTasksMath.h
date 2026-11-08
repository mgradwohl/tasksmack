#pragma once

// Pure helpers behind the Startup tab's scheduled tasks (#3424): which triggers count as starting at sign-in
// or boot, whose task is the current user's, which are Windows' own, the command a task's exec action runs,
// and the words a Task Scheduler error is shown in. No Windows header, so the tests build on every platform
// (tests/Platform/WindowsMath/); WindowsScheduledTasks.cpp checks the constants against taskschd.h.

#include "Platform/AsciiCase.h"
#include "Platform/IStartupProbe.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows::ScheduledTaskMath
{

/// TASK_TRIGGER_TYPE2's TASK_TRIGGER_BOOT: when the machine starts.
inline constexpr int TRIGGER_BOOT = 8;
/// TASK_TRIGGER_TYPE2's TASK_TRIGGER_LOGON: when a user (or a given user) signs in.
inline constexpr int TRIGGER_LOGON = 9;

/// HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), E_ACCESSDENIED.
inline constexpr std::uint32_t HRESULT_ACCESS_DENIED = 0x80070005U;
/// HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND): no task at that path.
inline constexpr std::uint32_t HRESULT_FILE_NOT_FOUND = 0x80070002U;
/// HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND): no folder at that path.
inline constexpr std::uint32_t HRESULT_PATH_NOT_FOUND = 0x80070003U;

/// Why one of Windows' own tasks can't be switched here: read-only, as the issue and Autoruns' default have it.
inline constexpr std::string_view MICROSOFT_TASK_REASON =
    "One of Windows' own scheduled tasks: TaskSmack leaves it as Windows set it (an administrator can change it in Task Scheduler)";

/// How deep the walk from the root folder goes: Task Scheduler's own folders are a few levels deep, so this
/// only stops a runaway walk.
inline constexpr int MAX_FOLDER_DEPTH = 16;

/// One of a task's triggers.
struct TaskTrigger
{
    int type = 0;        ///< TASK_TRIGGER_TYPE2.
    bool enabled = true; ///< ITrigger::get_Enabled: a disabled trigger never fires.
};

/// One registered task, as WindowsScheduledTasks reads it through the Task Scheduler API. Strings are UTF-8.
struct TaskRecord
{
    std::string path; ///< Its full path, "\Vendor\Updater" (IRegisteredTask::get_Path).
    bool enabled = true;
    std::vector<TaskTrigger> triggers;
    std::string execPath;      ///< The first exec action's Path, as written (environment variables, maybe quoted); empty when none.
    std::string execArguments; ///< That action's Arguments.
    /// That action's WorkingDirectory, as written: where a relative Path is found. Empty when none is set.
    std::string workingDirectory;
    /// RegistrationInfo's Author: a name, or an indirect string ("$(@%SystemRoot%\system32\x.dll,-600)").
    std::string author;
    /// The Principal's UserId: "DOMAIN\user", "user", a SID ("S-1-5-18"), "SYSTEM"; empty for a group's task.
    std::string userId;
};

/// One trigger as read: its @p type and @p enabled state, when each read (@p typeRead, @p enabledRead) succeeded.
/// Nullopt when either failed: a trigger whose state is a guess is left out, not taken for an enabled one.
[[nodiscard]] constexpr std::optional<TaskTrigger> readTrigger(bool typeRead, int type, bool enabledRead, bool enabled) noexcept
{
    if (!typeRead || !enabledRead)
    {
        return std::nullopt;
    }
    return TaskTrigger{.type = type, .enabled = enabled};
}

/// The program a task's exec action runs, where Task Scheduler finds it, or empty when TaskSmack can't tell (the
/// probe shows it unresolved). @p program is the action's Path with its variables expanded, quotes allowed;
/// @p workingDirectory its expanded WorkingDirectory: empty when the task sets none, nullopt when it sets one that
/// can't be told for this account (another account's %USERPROFILE%); @p systemDirectory Windows' System32.
/// @p exists says whether a file is at a path.
///   - An absolute path ("C:\A\a.exe", "\\server\share\a.exe") is the program.
///   - A drive-rooted one ("\Windows\System32\cmd.exe") is on the working directory's drive, with ".exe" added
///     as for a relative one, and can't be told when that drive isn't known (#3569).
///   - A relative or bare one is found in the working directory, with ".exe" added when it has no extension.
///     A relative path with a folder in it is that file there, found or not (it shows as missing).
///   - A bare name ("cmd") not found there is looked for in System32 only, never in TaskSmack's own working
///     directory or PATH, which aren't the task's.
///   - A relative or bare one whose working directory is unknown can't be told: that folder might hold its own
///     cmd.exe, so System32's isn't taken for it (#3569).
template<typename Exists>
[[nodiscard]] std::string
taskProgramPath(std::string_view program, std::optional<std::string_view> workingDirectory, std::string_view systemDirectory, Exists exists)
{
    if (program.size() >= 2 && program.front() == '"' && program.back() == '"')
    {
        program = program.substr(1, program.size() - 2);
    }
    if (program.empty())
    {
        return {};
    }
    const bool driveAbsolute = program.size() >= 3 && program[1] == ':' && (program[2] == '\\' || program[2] == '/');
    if (driveAbsolute || program.starts_with("\\\\") || program.starts_with("//"))
    {
        return std::string(program);
    }
    if (program.contains(':'))
    {
        return {}; // drive-relative ("C:a.exe"): relative to a current directory TaskSmack can't know
    }
    const std::string_view leaf = program.substr(program.find_last_of("\\/") + 1);
    const bool hasExtension = leaf.contains('.');
    if (program.front() == '\\' || program.front() == '/')
    {
        // Drive-rooted: on the working directory's drive ("C:\Vendor" gives "C:"), not inside that directory.
        const std::string_view folder = workingDirectory.value_or(std::string_view{});
        const char letter = Platform::foldAscii(folder.empty() ? '\0' : folder.front());
        if (folder.size() < 2 || folder[1] != ':' || letter < 'a' || letter > 'z')
        {
            return {};
        }
        // That file, or with ".exe" added when it has no extension, found or not, as a relative path is.
        std::string candidate = std::format("{}{}", folder.substr(0, 2), program);
        if (hasExtension || exists(candidate))
        {
            return candidate;
        }
        return candidate + ".exe";
    }
    if (!workingDirectory)
    {
        return {}; // a relative or bare name in a working directory TaskSmack can't tell
    }
    const bool hasFolder = program.find_first_of("\\/") != std::string_view::npos;
    // @p folder\program, whatever separators @p folder ends with.
    const auto join = [program](std::string_view folder)
    {
        while (!folder.empty() && (folder.back() == '\\' || folder.back() == '/'))
        {
            folder.remove_suffix(1);
        }
        return std::format("{}\\{}", folder, program);
    };
    // That file, or with ".exe" added when it isn't there and has no extension; empty when neither is.
    const auto findIn = [&](std::string_view folder) -> std::string
    {
        std::string candidate = join(folder);
        if (exists(candidate))
        {
            return candidate;
        }
        if (!hasExtension && exists(candidate + ".exe"))
        {
            return candidate + ".exe";
        }
        return {};
    };
    if (!workingDirectory->empty())
    {
        if (std::string found = findIn(*workingDirectory); !found.empty())
        {
            return found;
        }
        if (hasFolder)
        {
            return join(*workingDirectory) + (hasExtension ? "" : ".exe");
        }
    }
    if (!hasFolder && !systemDirectory.empty())
    {
        return findIn(systemDirectory);
    }
    return {};
}

/// Whether a trigger of @p type starts a task at sign-in or boot.
[[nodiscard]] constexpr bool isStartupTrigger(int type) noexcept
{
    return type == TRIGGER_LOGON || type == TRIGGER_BOOT;
}

/// Whether any of @p triggers that is enabled starts the task at sign-in or boot: the tasks the Startup tab lists.
[[nodiscard]] constexpr bool startsAtSignInOrBoot(std::span<const TaskTrigger> triggers) noexcept
{
    return std::ranges::any_of(triggers, [](const TaskTrigger& trigger) { return trigger.enabled && isStartupTrigger(trigger.type); });
}

/// Whether the task at @p path is one of Windows' own: under \Microsoft\, whatever the case.
[[nodiscard]] constexpr bool isMicrosoftTask(std::string_view path) noexcept
{
    constexpr std::string_view PREFIX = "\\Microsoft\\";
    return path.size() > PREFIX.size() && Platform::equalsIgnoreCase(path.substr(0, PREFIX.size()), PREFIX);
}

/// Whether a task whose principal is @p userId runs as the current user, known by @p currentUser ("DOMAIN\user",
/// "user" and the SID string; ASCII case-insensitive). A task with no UserId (a group's) runs for whoever signs
/// in, so it isn't the current user's own.
[[nodiscard]] constexpr bool runsAsUser(std::string_view userId, std::span<const std::string> currentUser) noexcept
{
    if (userId.empty())
    {
        return false;
    }
    return std::ranges::any_of(currentUser,
                               [userId](const std::string& name) { return !name.empty() && Platform::equalsIgnoreCase(userId, name); });
}

/// The command line an exec action runs: @p path, quoted unless it is already, then @p arguments. Empty when there is
/// no path (a COM handler task).
[[nodiscard]] inline std::string taskCommand(std::string_view path, std::string_view arguments)
{
    if (path.empty())
    {
        return {};
    }
    // Always quoted: the action keeps the program and its arguments apart, and an unquoted, extensionless Path
    // ("cmd" before "/c C:\Vendor\up.exe") would otherwise run into them when the command line is read back.
    std::string command = path.starts_with('"') ? std::string(path) : std::format("\"{}\"", path);
    if (!arguments.empty())
    {
        command += ' ';
        command += arguments;
    }
    return command;
}

/// Whether @p path names a %VARIABLE% whose value depends on the account it is expanded for (%USERPROFILE%,
/// %APPDATA%, %TEMP%...): anything but the machine-wide folders and settings, which are the same for every account
/// (%SystemRoot%, %ProgramFiles%, %ProgramData%...). A task that runs as another account expands those in its own
/// environment, so TaskSmack's own value would name the wrong file. ASCII case-insensitive; a lone % is no variable.
[[nodiscard]] constexpr bool usesAccountVariables(std::string_view path) noexcept
{
    constexpr std::array<std::string_view, 16> MACHINE_WIDE{
        "SystemRoot",
        "windir",
        "SystemDrive",
        "ProgramFiles",
        "ProgramFiles(x86)",
        "ProgramW6432",
        "CommonProgramFiles",
        "CommonProgramFiles(x86)",
        "CommonProgramW6432",
        "ProgramData",
        "ALLUSERSPROFILE",
        "PUBLIC",
        "ComSpec",
        "PROCESSOR_ARCHITECTURE",
        "NUMBER_OF_PROCESSORS",
        "OS",
    };
    std::size_t start = path.find('%');
    while (start != std::string_view::npos)
    {
        const std::size_t end = path.find('%', start + 1);
        if (end == std::string_view::npos)
        {
            return false;
        }
        const std::string_view name = path.substr(start + 1, end - start - 1);
        if (!name.empty() &&
            std::ranges::none_of(MACHINE_WIDE, [name](std::string_view known) { return Platform::equalsIgnoreCase(name, known); }))
        {
            return true;
        }
        start = path.find('%', end + 1);
    }
    return false;
}

/// The SHLoadIndirectString source an indirect Author names: "$(@%SystemRoot%\x.dll,-600)" gives
/// "@%SystemRoot%\x.dll,-600". Empty when @p author is plain text.
[[nodiscard]] inline std::string indirectSource(std::string_view author)
{
    if (author.size() > 4 && author.starts_with("$(@") && author.ends_with(')'))
    {
        return std::string(author.substr(2, author.size() - 3));
    }
    return {};
}

/// A failed Task Scheduler call's HRESULT, in words the result line shows as they stand.
[[nodiscard]] inline std::string taskErrorText(std::uint32_t hresult)
{
    if (hresult == HRESULT_ACCESS_DENIED)
    {
        return "Requires administrator";
    }
    if (hresult == HRESULT_FILE_NOT_FOUND || hresult == HRESULT_PATH_NOT_FOUND)
    {
        return "The scheduled task is no longer registered";
    }
    return std::format("Task Scheduler error 0x{:08X}", hresult);
}

/// The Startup tab's entry for @p task (one that startsAtSignInOrBoot): registered under its path and shown by its
/// own name (StartupMath::startupEntryName), its exec action's command, the current user's (@p currentUser) or the
/// machine's, and Windows' own tasks marked osComponent and read-only. The program and publisher are the probe's
/// to fill from the command; the Author stands in for a publisher the program doesn't name.
[[nodiscard]] inline StartupEntry taskEntry(const TaskRecord& task, std::span<const std::string> currentUser)
{
    StartupEntry entry;
    entry.registrationName = task.path;
    entry.command = taskCommand(task.execPath, task.execArguments);
    const bool own = runsAsUser(task.userId, currentUser);
    entry.location = own ? StartupLocation::ScheduledTaskUser : StartupLocation::ScheduledTaskMachine;
    entry.scope = own ? StartupScope::User : StartupScope::Machine;
    entry.enabled = task.enabled;
    entry.osComponent = isMicrosoftTask(task.path);
    if (entry.osComponent)
    {
        entry.notSwitchableReason = std::string(MICROSOFT_TASK_REASON);
    }
    return entry;
}

} // namespace Platform::Windows::ScheduledTaskMath
