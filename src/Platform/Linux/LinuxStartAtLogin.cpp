#include "LinuxStartAtLogin.h"

#include "Domain/StartAtLogin.h"
#include "Platform/IStartAtLogin.h"
#include "XdgAutostart.h"

#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace Platform
{

namespace StartAtLoginEntry
{

namespace
{

/// @p text with every @p from replaced by @p to.
[[nodiscard]] std::string replaceAll(std::string_view text, std::string_view from, std::string_view to)
{
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    for (std::size_t found = text.find(from); found != std::string_view::npos; found = text.find(from, at))
    {
        out.append(text.substr(at, found - at));
        out.append(to);
        at = found + from.size();
    }
    out.append(text.substr(at));
    return out;
}

/// @p command's text after its program: past the closing quote of a quoted one, else past the first space.
[[nodiscard]] std::string_view argumentsOf(std::string_view command)
{
    const std::size_t start = command.find_first_not_of(" \t");
    if (start == std::string_view::npos)
    {
        return {};
    }
    command.remove_prefix(start);
    std::size_t end = 0;
    if (command.front() == '"')
    {
        end = command.find('"', 1);
        end = (end == std::string_view::npos) ? command.size() : end + 1;
    }
    else
    {
        end = command.find_first_of(" \t");
        end = (end == std::string_view::npos) ? command.size() : end;
    }
    command.remove_prefix(end);
    const std::size_t first = command.find_first_not_of(" \t");
    return first == std::string_view::npos ? std::string_view{} : command.substr(first);
}

/// @p exec with the Desktop Entry string type's escapes undone: \s, \n, \t, \r and \\ (others are left as they are).
[[nodiscard]] std::string unescapeString(std::string_view exec)
{
    std::string out;
    out.reserve(exec.size());
    std::size_t i = 0;
    while (i < exec.size())
    {
        if (exec[i] != '\\' || i + 1 == exec.size())
        {
            out += exec[i];
            ++i;
            continue;
        }
        switch (exec[i + 1])
        {
        case 's':
            out += ' ';
            break;
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case 'r':
            out += '\r';
            break;
        case '\\':
            out += '\\';
            break;
        default:
            out += exec[i]; // not an escape: the backslash is kept, and the next character read as itself
            ++i;
            continue;
        }
        i += 2;
    }
    return out;
}

} // namespace

std::optional<std::string> execValue(std::string_view command)
{
    const std::optional<std::string> program = Domain::StartAtLogin::commandProgram(command);
    if (!program.has_value() || command.contains('\n'))
    {
        return std::nullopt;
    }
    // The quoting rule: the program in double quotes, \ " ` $ inside it escaped with a backslash.
    std::string quoted = "\"";
    for (const char c : *program)
    {
        if (c == '\\' || c == '"' || c == '`' || c == '$')
        {
            quoted += '\\';
        }
        quoted += c;
    }
    quoted += '"';
    const std::string_view arguments = argumentsOf(command);
    std::string exec = arguments.empty() ? quoted : quoted + " " + std::string(arguments);
    // A literal % is %% (field codes), then the string type's own escape doubles every backslash.
    exec = replaceAll(exec, "%", "%%");
    return replaceAll(exec, "\\", "\\\\");
}

std::optional<std::string> commandFromExec(std::string_view exec)
{
    const std::string text = unescapeString(exec);
    std::string_view rest(text);
    const std::size_t start = rest.find_first_not_of(" \t");
    if (start == std::string_view::npos)
    {
        return std::nullopt;
    }
    rest.remove_prefix(start);
    std::string program;
    if (rest.front() == '"')
    {
        std::size_t i = 1;
        bool closed = false;
        while (i < rest.size())
        {
            const char c = rest[i];
            if (c == '\\' && i + 1 < rest.size() && (rest[i + 1] == '\\' || rest[i + 1] == '"' || rest[i + 1] == '`' || rest[i + 1] == '$'))
            {
                program += rest[i + 1];
                i += 2;
                continue;
            }
            if (c == '"')
            {
                closed = true;
                break;
            }
            program += c;
            ++i;
        }
        if (!closed)
        {
            return std::nullopt;
        }
        rest.remove_prefix(i + 1);
    }
    else
    {
        const std::size_t end = rest.find_first_of(" \t");
        program = std::string(rest.substr(0, end));
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end);
    }
    const std::size_t first = rest.find_first_not_of(" \t");
    const std::string arguments = (first == std::string_view::npos) ? std::string{} : replaceAll(rest.substr(first), "%%", "%");
    std::string command = "\"" + replaceAll(program, "%%", "%") + "\"";
    if (!arguments.empty())
    {
        command += " " + arguments;
    }
    return command;
}

std::string entryText(std::string_view exec)
{
    std::string text = "[Desktop Entry]\n"
                       "Type=Application\n"
                       "Name=TaskSmack\n"
                       "Comment=Start TaskSmack minimized when you sign in\n"
                       "Icon=app.tasksmack.TaskSmack\n"
                       "Terminal=false\n"
                       "X-GNOME-Autostart-enabled=true\n"
                       "Exec=";
    text += exec;
    text += '\n';
    return text;
}

std::filesystem::path autostartDirectory(const char* xdgConfigHome, const char* home)
{
    // The XDG Base Directory spec ignores a relative $XDG_CONFIG_HOME.
    if (xdgConfigHome != nullptr && xdgConfigHome[0] == '/')
    {
        return std::filesystem::path(xdgConfigHome) / "autostart";
    }
    if (home != nullptr && home[0] == '/')
    {
        return std::filesystem::path(home) / ".config" / "autostart";
    }
    return {};
}

} // namespace StartAtLoginEntry

namespace
{

[[nodiscard]] std::filesystem::path environmentAutostartDirectory()
{
    // NOLINTBEGIN(concurrency-mt-unsafe) - read at construction, on the composition root's thread
    return StartAtLoginEntry::autostartDirectory(std::getenv("XDG_CONFIG_HOME"), std::getenv("HOME"));
    // NOLINTEND(concurrency-mt-unsafe)
}

[[nodiscard]] std::string runningExecutable()
{
    std::error_code error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::string{} : self.string();
}

constexpr std::string_view NO_HOME = "Your home folder isn't known ($HOME is unset), so there is no autostart folder";

} // namespace

LinuxStartAtLogin::LinuxStartAtLogin() : LinuxStartAtLogin(environmentAutostartDirectory(), runningExecutable())
{}

LinuxStartAtLogin::LinuxStartAtLogin(std::filesystem::path autostartDirectory, std::string executable)
    : m_Directory(std::move(autostartDirectory)), m_Executable(std::move(executable))
{}

std::filesystem::path LinuxStartAtLogin::entryPath() const
{
    return m_Directory / StartAtLoginEntry::FILE_NAME;
}

std::expected<StartAtLoginFacts, std::string> LinuxStartAtLogin::read()
{
    if (m_Directory.empty())
    {
        return std::unexpected(std::string(NO_HOME));
    }
    StartAtLoginFacts facts;
    facts.executable = m_Executable;
    // An installed copy (the .deb's /usr/bin, or /opt); one elsewhere is a portable copy Settings warns about.
    facts.installed = m_Executable.starts_with("/usr/") || m_Executable.starts_with("/opt/");
    facts.caseInsensitivePaths = false;

    const std::filesystem::path path = entryPath();
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        if (error)
        {
            return std::unexpected("The autostart entry couldn't be checked: " + error.message());
        }
        return facts; // no entry: off
    }
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::unexpected("The autostart entry " + path.string() + " couldn't be read");
    }
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const std::optional<XdgAutostart::Entry> entry = XdgAutostart::parse(text);
    // Switched off in the desktop's own startup settings (Hidden=true, X-GNOME-Autostart-enabled=false): off.
    if (!entry.has_value() || !entry->enabled())
    {
        return facts;
    }
    facts.command = entry->exec.empty() ? std::string{} : StartAtLoginEntry::commandFromExec(entry->exec).value_or(std::string{});
    return facts;
}

std::expected<void, std::string> LinuxStartAtLogin::write(std::string_view command)
{
    if (m_Directory.empty())
    {
        return std::unexpected(std::string(NO_HOME));
    }
    const std::optional<std::string> exec = StartAtLoginEntry::execValue(command);
    if (!exec.has_value())
    {
        return std::unexpected(std::string("This copy's path can't be written to an autostart entry"));
    }
    std::error_code error;
    std::filesystem::create_directories(m_Directory, error);
    if (error)
    {
        return std::unexpected("The autostart folder " + m_Directory.string() + " couldn't be made: " + error.message());
    }
    // Written beside the entry and renamed over it, so a desktop reading it at sign-in never sees half a file.
    const std::filesystem::path path = entryPath();
    std::filesystem::path partial = path;
    partial += ".partial";
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        file << StartAtLoginEntry::entryText(*exec);
        file.close();
        if (!file)
        {
            std::filesystem::remove(partial, error);
            return std::unexpected("The autostart entry " + path.string() + " couldn't be written");
        }
    }
    std::filesystem::rename(partial, path, error);
    if (error)
    {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        return std::unexpected("The autostart entry " + path.string() + " couldn't be written: " + error.message());
    }
    return {};
}

std::expected<void, std::string> LinuxStartAtLogin::remove()
{
    if (m_Directory.empty())
    {
        return std::unexpected(std::string(NO_HOME));
    }
    std::error_code error;
    std::filesystem::remove(entryPath(), error); // false, not an error, when it isn't there
    if (error)
    {
        return std::unexpected("The autostart entry couldn't be removed: " + error.message());
    }
    return {};
}

} // namespace Platform
