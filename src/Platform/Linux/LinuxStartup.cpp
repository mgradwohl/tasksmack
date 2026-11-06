#include "LinuxStartup.h"

#include "Domain/SamplingConfig.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/Linux/CommandOutput.h"
#include "Platform/Linux/CronTab.h"
#include "Platform/Linux/DesktopEntryIcons.h"
#include "Platform/Linux/SystemdUnitFile.h"
#include "Platform/Linux/XdgAutostart.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace Platform
{

namespace
{

/// The largest autostart file read: they are a few hundred bytes; anything far larger isn't one.
constexpr std::uintmax_t MAX_FILE_BYTES = std::uintmax_t{64} * 1024U;

/// The value of environment variable @p name, or empty. Read when the probe or actions are made, before
/// they are used on a worker.
[[nodiscard]] std::string environment(const char* name)
{
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe) - see above
    return value != nullptr ? std::string(value) : std::string{};
}

[[nodiscard]] XdgAutostart::Dirs dirsFromEnvironment()
{
    return XdgAutostart::dirs(environment("HOME"), environment("XDG_CONFIG_HOME"), environment("XDG_CONFIG_DIRS"));
}

/// @p path's text, or nullopt when it can't be read or is too large to be an autostart file.
[[nodiscard]] std::optional<std::string> readFile(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > MAX_FILE_BYTES)
    {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

/// The autostart files (*.desktop) in @p dir, by file name.
[[nodiscard]] std::map<std::string, std::filesystem::path> desktopFiles(const std::filesystem::path& dir)
{
    std::map<std::string, std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::filesystem::path& path = it->path();
        if (path.extension() == ".desktop" && it->is_regular_file(ec))
        {
            files.emplace(path.filename().string(), path);
        }
    }
    return files;
}

/// Whether @p word is an environment assignment ("LD_LIBRARY_PATH=/opt/x/lib"): a shell name before its '='.
[[nodiscard]] bool isAssignment(std::string_view word)
{
    const std::size_t equals = word.find('=');
    if (equals == std::string_view::npos || equals == 0)
    {
        return false;
    }
    const std::string_view name = word.substr(0, equals);
    const auto nameChar = [](char c)
    {
        return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    return std::ranges::all_of(name, nameChar) && (name.front() < '0' || name.front() > '9');
}

/// The program @p entry runs: TryExec, else the first word of Exec past any VAR=value and an `env` with
/// its options, resolved on $PATH (@p searchPath) when it has no '/'. Empty when there's none.
[[nodiscard]] std::string programOf(const XdgAutostart::Entry& entry, std::string_view searchPath)
{
    std::string program = entry.tryExec;
    if (program.empty())
    {
        bool afterEnv = false;
        bool skipNext = false; // env's -u NAME, -C DIR
        for (const std::string& word : DesktopEntryIcons::splitExec(entry.exec))
        {
            if (skipNext)
            {
                skipNext = false;
                continue;
            }
            if (afterEnv && word.starts_with('-'))
            {
                skipNext = word == "-u" || word == "-C" || word == "--unset" || word == "--chdir";
                continue;
            }
            if (isAssignment(word))
            {
                continue;
            }
            if (!afterEnv && (word == "env" || word.ends_with("/env")))
            {
                afterEnv = true;
                continue;
            }
            program = word;
            break;
        }
    }
    if (program.empty() || program.contains('/'))
    {
        return program;
    }
    for (const std::string& dir : XdgAutostart::splitList(searchPath, ':'))
    {
        std::error_code ec;
        const std::filesystem::path candidate = std::filesystem::path(dir) / program;
        if (std::filesystem::is_regular_file(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0)
        {
            return candidate.string();
        }
    }
    return program; // not found: left as written, and Missing
}

/// Whether @p program (programOf()'s answer) exists: Unresolved when there's none, Missing when it
/// wasn't found on $PATH or isn't there.
[[nodiscard]] StartupTargetState targetState(const std::string& program)
{
    if (program.empty())
    {
        return StartupTargetState::Unresolved;
    }
    std::error_code ec;
    return program.front() == '/' && std::filesystem::exists(program, ec) ? StartupTargetState::Present : StartupTargetState::Missing;
}

/// Why an entry the current desktop doesn't start can't be switched here.
[[nodiscard]] std::string otherDesktopReason(const XdgAutostart::Entry& entry)
{
    std::string desktops;
    for (const std::string& desktop : entry.onlyShowIn)
    {
        desktops += desktops.empty() ? desktop : ", " + desktop;
    }
    return desktops.empty() ? std::string("This desktop doesn't start it (NotShowIn)") : "Starts only in " + desktops;
}

/// Writes @p text to @p path through a temporary file renamed over it, so a failure leaves the old file.
[[nodiscard]] bool writeFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::path temporary = path;
    temporary += std::format(".tasksmack-{}.tmp", ::getpid());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(text.data(), static_cast<std::streamsize>(text.size())) || !file.flush())
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code ec;
    // A replaced file keeps its permissions; a new one gets the umask's, as any file the user makes.
    if (const std::filesystem::file_status old = std::filesystem::status(path, ec); !ec && std::filesystem::is_regular_file(old))
    {
        std::filesystem::permissions(temporary, old.permissions(), ec);
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec)
    {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

/// The program a cron job starts and whether it is there.
struct CronTarget
{
    std::string program; ///< Its path when found; as written when Missing; empty when Unresolved
    std::string stem;    ///< What the row is named after: the program's file name, or the command's first word
    StartupTargetState state = StartupTargetState::Unresolved;
};

/// Whether @p shell reads a command as POSIX sh does, so CronTab::program() reads it right.
[[nodiscard]] bool posixShell(std::string_view shell)
{
    const std::string name = std::filesystem::path(shell).filename().string();
    return name == "sh" || name == "dash" || name == "bash" || name == "ksh" || name == "mksh" || name == "zsh" || name == "busybox";
}

/// Where cron's shell finds @p job's program: its first command word (CronTab::program()), looked up on the job's
/// own PATH (an inline `PATH=... cmd`, else the table's last PATH=, else cron's default) when it has no '/'. A
/// builtin, shell syntax not followed, another kind of shell, a relative path (it runs from the user's home) or a
/// PATH with relative or unreadable directories leaves it Unresolved rather than Missing: only what cron would
/// certainly not find is Missing.
[[nodiscard]] CronTarget cronTarget(const CronTab::Entry& job)
{
    CronTarget target;
    const CronTab::Program found = CronTab::program(job.command);
    if (found.kind != CronTab::Program::Kind::Executable || !posixShell(job.shell))
    {
        target.stem = found.word.empty() ? job.command.substr(0, job.command.find_first_of(" \t")) : found.word;
        return target;
    }
    target.stem = std::filesystem::path(found.word).filename().string();
    if (found.word.contains('/'))
    {
        if (found.word.front() != '/')
        {
            return target; // relative to the directory the job runs in
        }
        std::error_code ec;
        const bool there = std::filesystem::exists(found.word, ec);
        if (ec)
        {
            return target; // a directory TaskSmack can't search (/root/backup.sh): cron's may, so not Missing
        }
        target.program = found.word;
        target.state = there ? StartupTargetState::Present : StartupTargetState::Missing;
        return target;
    }
    bool uncertain = false;
    // env -i or env -u PATH: execvp() then searches its default path rather than the one the job inherits.
    constexpr std::string_view EXECVP_DEFAULT_PATH = "/bin:/usr/bin";
    std::string_view path = job.path;
    if (found.path.has_value())
    {
        path = *found.path;
    }
    else if (found.pathRemoved)
    {
        path = EXECVP_DEFAULT_PATH;
    }
    for (;;)
    {
        const std::size_t colon = path.find(':');
        const std::string_view dir = path.substr(0, colon);
        if (dir.empty() || dir.front() != '/')
        {
            uncertain = true; // the current directory, or one relative to it
        }
        else
        {
            std::error_code ec;
            const std::filesystem::path candidate = std::filesystem::path(dir) / found.word;
            const std::filesystem::file_status status = std::filesystem::status(candidate, ec);
            constexpr auto ANY_EXEC =
                std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;
            if (std::filesystem::is_regular_file(status) && (status.permissions() & ANY_EXEC) != std::filesystem::perms::none)
            {
                if (uncertain)
                {
                    return target; // an earlier directory, relative or unreadable, may hold the one cron runs
                }
                target.program = candidate.string();
                target.state = StartupTargetState::Present;
                return target;
            }
            uncertain = uncertain || ec == std::errc::permission_denied; // a directory TaskSmack can't search
        }
        if (colon == std::string_view::npos)
        {
            break;
        }
        path.remove_prefix(colon + 1);
    }
    if (!uncertain)
    {
        target.program = found.word; // left as written, and Missing
        target.state = StartupTargetState::Missing;
    }
    return target;
}

} // namespace

SystemdUserDirs systemdUserDirs(std::string_view home, std::string_view xdgConfigHome)
{
    SystemdUserDirs dirs;
    if (!xdgConfigHome.empty() && xdgConfigHome.front() == '/')
    {
        dirs.user = std::filesystem::path(xdgConfigHome) / "systemd" / "user";
    }
    else if (!home.empty() && home.front() == '/')
    {
        dirs.user = std::filesystem::path(home) / ".config" / "systemd" / "user";
    }
    dirs.system = {"/etc/systemd/user"};
    if (!dirs.user.empty())
    {
        dirs.unitPath.push_back(dirs.user);
    }
    dirs.unitPath.insert(dirs.unitPath.end(), {"/etc/systemd/user", "/usr/local/lib/systemd/user", "/usr/lib/systemd/user"});
    return dirs;
}

LinuxStartupProbe::LinuxStartupProbe()
    : LinuxStartupProbe(dirsFromEnvironment(),
                        environment("XDG_CURRENT_DESKTOP"),
                        environment("PATH"),
                        systemdUserDirs(environment("HOME"), environment("XDG_CONFIG_HOME")),
                        cronSources())
{}

LinuxStartupProbe::LinuxStartupProbe(XdgAutostart::Dirs dirs, std::string currentDesktops, std::string searchPath)
    : LinuxStartupProbe(std::move(dirs), std::move(currentDesktops), std::move(searchPath), SystemdUserDirs{})
{}

LinuxStartupProbe::LinuxStartupProbe(XdgAutostart::Dirs dirs, std::string currentDesktops, std::string searchPath, SystemdUserDirs systemd)
    : LinuxStartupProbe(std::move(dirs), std::move(currentDesktops), std::move(searchPath), std::move(systemd), CronSources{})
{}

LinuxStartupProbe::LinuxStartupProbe(
    XdgAutostart::Dirs dirs, std::string currentDesktops, std::string searchPath, SystemdUserDirs systemd, CronSources cron)
    : m_Dirs(std::move(dirs)),
      m_Systemd(std::move(systemd)),
      m_Cron(std::move(cron)),
      m_CurrentDesktops(std::move(currentDesktops)),
      m_SearchPath(std::move(searchPath))
{}

bool cronTableFileName(std::string_view name) noexcept
{
    const auto allowed = [](char c)
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    };
    return !name.empty() && std::ranges::all_of(name, allowed);
}

CronSources cronSources()
{
    // `crontab -l` re-run at most every STARTUP_CRON_REFRESH_SECONDS: the Startup tab samples every few seconds, and a
    // crontab changes only when its owner edits it. Shared by copies of the function, guarded for the sampler thread.
    struct Cache
    {
        std::mutex mutex;
        std::optional<std::chrono::steady_clock::time_point> readAt;
        std::optional<std::string> text;
    };
    const auto cache = std::make_shared<Cache>();
    CronSources sources;
    sources.userCrontab = [cache] -> std::optional<std::string>
    {
        const std::scoped_lock lock(cache->mutex);
        const auto now = std::chrono::steady_clock::now();
        if (!cache->readAt.has_value() || now - *cache->readAt >= std::chrono::seconds(Domain::Sampling::STARTUP_CRON_REFRESH_SECONDS))
        {
            // Exit 1 with "no crontab for <user>" on stderr when there's none: no jobs, not an error to show.
            const CommandOutput run = runForOutput({"crontab", "-l"}, std::chrono::seconds(2), std::size_t{256} * 1024);
            cache->text = (run.started && run.exitCode == 0) ? std::optional<std::string>(run.output) : std::nullopt;
            cache->readAt = now;
        }
        return cache->text;
    };
    sources.systemTables.emplace_back("/etc/crontab");
    sources.systemTableDirs.emplace_back("/etc/cron.d");
    return sources;
}

void LinuxStartupProbe::addCronJobs(std::vector<StartupEntry>& entries) const
{
    // ignoredBecause: why cron skips the whole table (a system table it won't trust), else empty.
    const auto add = [&entries](const CronTab::Entry& job, const std::string& table, bool system, std::string_view ignoredBecause)
    {
        const CronTarget found = cronTarget(job);
        StartupEntry entry;
        entry.name =
            std::format("{} (cron, {}{})", found.stem, CronTab::describeSchedule(job.schedule), job.user.empty() ? "" : ", as " + job.user);
        entry.programName = entry.name;
        entry.registrationName = std::format("{} line {}", table, job.line); // the line identifies it in its table
        entry.command = job.command;
        entry.executablePath = found.program;
        entry.sourcePath = system ? table : std::string{};
        entry.location = system ? StartupLocation::CronSystem : StartupLocation::CronUser;
        entry.scope = system ? StartupScope::Machine : StartupScope::User;
        entry.enabled = ignoredBecause.empty(); // a job line is on; a commented-out one isn't listed
        if (!ignoredBecause.empty())
        {
            entry.notSwitchableReason = std::format("cron ignores {}: it {}", table, ignoredBecause);
        }
        else
        {
            entry.notSwitchableReason = system ? std::format("A system cron job, in {}: an administrator changes it", table)
                                               : std::string("A cron job: change it with crontab -e");
        }
        entry.target = found.state;
        entries.push_back(std::move(entry));
    };
    if (m_Cron.userCrontab)
    {
        if (const std::optional<std::string> text = m_Cron.userCrontab())
        {
            for (const CronTab::Entry& job : CronTab::parse(*text, /*withUser=*/false))
            {
                add(job, "crontab", false, {});
            }
        }
    }
    // The directories are listed here, on the sampler thread, each sample: a table added since shows at once.
    std::vector<std::filesystem::path> tables = m_Cron.systemTables;
    for (const std::filesystem::path& dir : m_Cron.systemTableDirs)
    {
        std::vector<std::filesystem::path> found;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code typeError;
            if (cronTableFileName(it->path().filename().string()) && it->is_regular_file(typeError))
            {
                found.push_back(it->path());
            }
        }
        std::ranges::sort(found);
        tables.insert(tables.end(), found.begin(), found.end());
    }
    for (const std::filesystem::path& table : tables)
    {
        if (const std::optional<std::string> text = readFile(table))
        {
            // cron (Debian's and cronie) skips a system table not owned by root or writable by group or others.
            std::string_view ignoredBecause;
            struct stat info{};
            if (::stat(table.c_str(), &info) == 0)
            {
                if (info.st_uid != m_Cron.systemTableOwner)
                {
                    ignoredBecause = "isn't owned by root";
                }
                else if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0)
                {
                    ignoredBecause = "can be written by its group or others";
                }
            }
            for (const CronTab::Entry& job : CronTab::parse(*text, /*withUser=*/true))
            {
                add(job, table.string(), true, ignoredBecause);
            }
        }
    }
}

void LinuxStartupProbe::addSystemdUnits(std::vector<StartupEntry>& entries) const
{
    // Each unit name once: a user's link wins over the same unit enabled for every user.
    struct Found
    {
        std::filesystem::path link;
        bool system = false;
    };
    std::map<std::string, Found> units;
    const auto scan = [&units](const std::filesystem::path& dir, bool system)
    {
        for (const char* wants : {"default.target.wants", "graphical-session.target.wants", "timers.target.wants"})
        {
            std::error_code ec;
            std::filesystem::directory_iterator it(dir / wants, ec);
            for (; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
            {
                const std::string name = it->path().filename().string();
                if (name.ends_with(".service") || name.ends_with(".timer"))
                {
                    units.try_emplace(name, Found{.link = it->path(), .system = system});
                }
            }
        }
    };
    if (!m_Systemd.user.empty())
    {
        scan(m_Systemd.user, false);
    }
    for (const std::filesystem::path& dir : m_Systemd.system)
    {
        scan(dir, true);
    }
    for (const auto& [name, found] : units)
    {
        const bool timer = name.ends_with(".timer");
        const SystemdUnitFile::Unit unit = SystemdUnitFile::parse(readFile(found.link).value_or(std::string{}));
        // A timer starts its service: Unit=, else the timer's own name with .service.
        std::string command = unit.execStart;
        if (timer)
        {
            const std::string service =
                unit.timerUnit.empty() ? name.substr(0, name.size() - std::string_view(".timer").size()) + ".service" : unit.timerUnit;
            for (const std::filesystem::path& dir : m_Systemd.unitPath)
            {
                if (const std::optional<std::string> text = readFile(dir / service))
                {
                    command = SystemdUnitFile::parse(*text).execStart;
                    break;
                }
            }
        }
        StartupEntry entry;
        const std::string stem = name.substr(0, name.rfind('.'));
        entry.name = unit.description.empty() ? stem : unit.description;
        if (timer && !unit.schedule.empty())
        {
            entry.name += " (timer, " + unit.schedule + ")";
        }
        entry.programName = entry.name;
        entry.registrationName = name;
        entry.command = command;
        entry.executablePath = command.substr(0, command.find(' '));
        entry.sourcePath = found.link.string();
        entry.location = found.system ? StartupLocation::SystemdSystemUnit : StartupLocation::SystemdUserUnit;
        entry.scope = found.system ? StartupScope::Machine : StartupScope::User;
        entry.enabled = true; // linked into a .wants directory: that is what "enabled" is
        entry.notSwitchableReason = found.system ? "Enabled for every user in /etc/systemd/user"
                                                 : "A systemd unit: switch it with systemctl --user disable " + name;
        entry.target = entry.executablePath.empty() ? StartupTargetState::Unresolved : targetState(entry.executablePath);
        entries.push_back(std::move(entry));
    }
}

StartupCapabilities LinuxStartupProbe::capabilities() const
{
    return {
        .canEnumerate = true,
        .hasEnabledState = true,
        .hasDisabledTime = false,
        .hasPublisher = false,
        .canResolveShortcuts = false,
        .unavailableReason = {},
    };
}

std::vector<StartupEntry> LinuxStartupProbe::enumerate()
{
    // Each file name once: the user's directory first, then the system ones in order.
    const std::map<std::string, std::filesystem::path> userFiles =
        m_Dirs.user.empty() ? std::map<std::string, std::filesystem::path>{} : desktopFiles(m_Dirs.user);
    std::map<std::string, std::filesystem::path> systemFiles;
    for (const std::filesystem::path& dir : m_Dirs.system)
    {
        systemFiles.merge(desktopFiles(dir)); // an earlier directory's file of the same name is kept
    }
    std::map<std::string, std::filesystem::path> names = userFiles;
    names.insert(systemFiles.begin(), systemFiles.end());

    std::vector<StartupEntry> entries;
    for (const auto& [fileName, path] : names)
    {
        const std::optional<std::string> text = readFile(path);
        const std::optional<XdgAutostart::Entry> parsed = text ? XdgAutostart::parse(*text) : std::nullopt;
        if (!parsed || !parsed->application)
        {
            continue; // unreadable, or not an application: the session ignores it too
        }
        const bool fromSystem = systemFiles.contains(fileName);
        // A user stub with no Exec ("[Desktop Entry]\nHidden=true", the usual way to hide a system entry)
        // replaces the system file for the session; the list still names what the system file starts.
        XdgAutostart::Entry shown = *parsed;
        if (fromSystem && parsed->exec.empty() && userFiles.contains(fileName))
        {
            const std::optional<std::string> systemText = readFile(systemFiles.at(fileName));
            if (const std::optional<XdgAutostart::Entry> system = systemText ? XdgAutostart::parse(*systemText) : std::nullopt)
            {
                shown.name = system->name;
                shown.exec = system->exec;
                shown.tryExec = system->tryExec;
            }
        }
        StartupEntry entry;
        entry.name = shown.name.empty() ? fileName.substr(0, fileName.size() - std::string_view(".desktop").size()) : shown.name;
        entry.programName = entry.name;
        entry.registrationName = fileName;
        entry.command = shown.exec;
        entry.executablePath = programOf(shown, m_SearchPath);
        entry.sourcePath = path.string(); // the file the session reads: the user's, when there is one
        // iconPath stays empty: Icon= names a theme icon, and the icon source finds the program's own
        // through executablePath.
        // A system file starts for every user, even when this user's copy switches it.
        entry.location = fromSystem ? StartupLocation::XdgAutostartSystem : StartupLocation::XdgAutostartUser;
        entry.scope = fromSystem ? StartupScope::Machine : StartupScope::User;
        const bool startsHere = XdgAutostart::startsIn(*parsed, m_CurrentDesktops);
        // The session drops a file with no Exec: the user's stub hides the system entry whatever it says.
        entry.enabled = parsed->enabled() && startsHere && !parsed->exec.empty();
        if (!startsHere)
        {
            entry.notSwitchableReason = otherDesktopReason(*parsed);
        }
        else if (parsed->sessionComponent || parsed->noDisplay)
        {
            // Accessibility, settings daemons and the like: switching them off breaks the session, and
            // the desktops' own startup settings don't offer it.
            entry.notSwitchableReason = "Part of the desktop session";
        }
        entry.target = targetState(entry.executablePath);
        entries.push_back(std::move(entry));
    }
    addSystemdUnits(entries); // #3082
    addCronJobs(entries);     // #3425
    return entries;
}

LinuxStartupActions::LinuxStartupActions() : LinuxStartupActions(dirsFromEnvironment())
{}

LinuxStartupActions::LinuxStartupActions(XdgAutostart::Dirs dirs) : m_Dirs(std::move(dirs))
{}

StartupActionCapabilities LinuxStartupActions::capabilities() const
{
    // Every switch is a file in the user's own directory, so no entry needs root, a system one included.
    return {.canSetEnabled = !m_Dirs.user.empty(), .elevated = false, .machineEntriesNeedElevation = false};
}

StartupActionResult LinuxStartupActions::setEnabled(const StartupEntry& entry, bool enabled)
{
    const std::string& fileName = entry.registrationName;
    if (m_Dirs.user.empty())
    {
        return StartupActionResult::failed("Neither XDG_CONFIG_HOME nor HOME is set, so there's no autostart folder to write");
    }
    if (fileName.empty() || fileName.contains('/') || !fileName.ends_with(".desktop"))
    {
        return StartupActionResult::failed("This isn't an autostart file");
    }
    const std::filesystem::path userFile = m_Dirs.user / fileName;
    std::optional<std::filesystem::path> systemFile;
    for (const std::filesystem::path& dir : m_Dirs.system)
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(dir / fileName, ec))
        {
            systemFile = dir / fileName;
            break;
        }
    }
    std::error_code ec;
    // A symlinked copy (dotfiles managed with stow or chezmoi) would be replaced by a plain file: say so
    // rather than detach it.
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(userFile, ec)))
    {
        return StartupActionResult::failed(std::format("{} is a symbolic link; change the file it points to", userFile.string()));
    }
    const bool hasUserFile = std::filesystem::is_regular_file(userFile, ec);
    std::optional<std::string> current;
    if (hasUserFile)
    {
        current = readFile(userFile);
    }
    else if (systemFile)
    {
        current = readFile(*systemFile);
    }
    if (!current)
    {
        return StartupActionResult::failed(std::format("{} couldn't be read", hasUserFile ? userFile.string() : fileName));
    }
    std::optional<std::string> switched = XdgAutostart::withEnabled(*current, enabled);
    if (!switched)
    {
        return StartupActionResult::failed(std::format("{} has no [Desktop Entry] group", fileName));
    }
    const std::optional<std::string> systemText = systemFile ? readFile(*systemFile) : std::nullopt;
    const std::optional<XdgAutostart::Entry> systemEntry = systemText ? XdgAutostart::parse(*systemText) : std::nullopt;
    const bool switchedHasExec = !XdgAutostart::parse(*switched).value_or(XdgAutostart::Entry{}).exec.empty();
    // A user stub with no Exec ("[Desktop Entry]\nHidden=true") hides the system entry however it is set:
    // switched on, it becomes a copy of the system file instead.
    if (enabled && !switchedHasExec && systemText && systemEntry && !systemEntry->exec.empty())
    {
        switched = XdgAutostart::withEnabled(*systemText, true);
    }
    if (enabled && (!switched || XdgAutostart::parse(*switched).value_or(XdgAutostart::Entry{}).exec.empty()))
    {
        return StartupActionResult::failed(std::format("{} names no program to start (no Exec)", fileName));
    }
    // Back on, a copy that only switched a system entry off goes, so the system file applies again.
    if (enabled && hasUserFile && systemText && systemEntry)
    {
        if (systemEntry->enabled() && XdgAutostart::withEnabled(*systemText, true) == switched)
        {
            if (std::filesystem::remove(userFile, ec); ec)
            {
                return StartupActionResult::failed(std::format("{} couldn't be removed: {}", userFile.string(), ec.message()));
            }
            return StartupActionResult::succeeded();
        }
    }
    if (std::filesystem::create_directories(m_Dirs.user, ec); ec)
    {
        return StartupActionResult::failed(std::format("{} couldn't be made: {}", m_Dirs.user.string(), ec.message()));
    }
    if (!writeFile(userFile, *switched))
    {
        return StartupActionResult::failed(std::format("{} couldn't be written", userFile.string()));
    }
    return StartupActionResult::succeeded();
}

} // namespace Platform
