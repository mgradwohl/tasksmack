#include "LinuxStartup.h"

#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/Linux/DesktopEntryIcons.h"
#include "Platform/Linux/SystemdUnitFile.h"
#include "Platform/Linux/XdgAutostart.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
                        systemdUserDirs(environment("HOME"), environment("XDG_CONFIG_HOME")))
{}

LinuxStartupProbe::LinuxStartupProbe(XdgAutostart::Dirs dirs, std::string currentDesktops, std::string searchPath)
    : LinuxStartupProbe(std::move(dirs), std::move(currentDesktops), std::move(searchPath), SystemdUserDirs{})
{}

LinuxStartupProbe::LinuxStartupProbe(XdgAutostart::Dirs dirs, std::string currentDesktops, std::string searchPath, SystemdUserDirs systemd)
    : m_Dirs(std::move(dirs)),
      m_Systemd(std::move(systemd)),
      m_CurrentDesktops(std::move(currentDesktops)),
      m_SearchPath(std::move(searchPath))
{}

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
    return {.canEnumerate = true,
            .hasEnabledState = true,
            .hasDisabledTime = false,
            .hasPublisher = false,
            .canResolveShortcuts = false,
            .unavailableReason = {}};
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
