#pragma once

/// @file CronTab.h
/// @brief Reading cron tables for the Linux Startup tab (#3425): the user's crontab (`crontab -l`) and the system's
/// (/etc/crontab, /etc/cron.d/*), which add a user column, and the program a job's shell command starts, read
/// without running anything. Pure text parsing; no I/O.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::CronTab
{

/// The PATH cron gives a job whose table sets none: Debian's and Ubuntu's cron, the documented target, sets "PATH is
/// set to /usr/bin:/bin" (crontab(5)); cronie (Fedora) uses the same _PATH_DEFPATH.
inline constexpr std::string_view DEFAULT_PATH = "/usr/bin:/bin";
/// The shell cron runs a job's command with, unless the table sets SHELL (crontab(5)).
inline constexpr std::string_view DEFAULT_SHELL = "/bin/sh";

/// One job line of a cron table.
struct Entry
{
    std::string schedule;                           ///< "@reboot", "@daily", ... or the five fields as written ("*/5 * * * *")
    std::string user;                               ///< The user it runs as: the system tables' sixth column; empty in a user's own crontab
    std::string command;                            ///< The rest of the line, as cron hands it to the shell
    std::size_t line = 0;                           ///< 1-based line number in its table, which identifies it there
    std::string path = std::string(DEFAULT_PATH);   ///< The job's PATH: the last PATH= above it in its table, else cron's
    std::string shell = std::string(DEFAULT_SHELL); ///< The job's shell: the last SHELL= above it, else /bin/sh
};

namespace Detail
{

[[nodiscard]] constexpr bool isBlank(char c) noexcept
{
    return c == ' ' || c == '\t';
}

[[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && (isBlank(text.front()) || text.front() == '\r'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (isBlank(text.back()) || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }
    return text;
}

/// The next whitespace-separated word of @p text, removed from it; empty at the end.
[[nodiscard]] constexpr std::string_view nextWord(std::string_view& text)
{
    while (!text.empty() && isBlank(text.front()))
    {
        text.remove_prefix(1);
    }
    std::size_t end = 0;
    while (end < text.size() && !isBlank(text[end]))
    {
        ++end;
    }
    const std::string_view word = text.substr(0, end);
    text.remove_prefix(end);
    return word;
}

/// Whether @p line sets an environment variable ("MAILTO=x", "PATH = /bin") rather than naming a job: a name of
/// letters, digits and underscores, then '='.
[[nodiscard]] constexpr bool isAssignment(std::string_view line) noexcept
{
    std::size_t i = 0;
    while (i < line.size() && (line[i] == '_' || (line[i] >= 'A' && line[i] <= 'Z') || (line[i] >= 'a' && line[i] <= 'z') ||
                               (i > 0 && line[i] >= '0' && line[i] <= '9')))
    {
        ++i;
    }
    if (i == 0)
    {
        return false;
    }
    while (i < line.size() && isBlank(line[i]))
    {
        ++i;
    }
    return i < line.size() && line[i] == '=';
}

/// The name and value of an assignment line (isAssignment()), as cron reads it: blanks around the '=' and at the
/// end dropped, and the value's matching single or double quotes removed (crontab(5)).
struct Assignment
{
    std::string_view name;
    std::string_view value;
};

[[nodiscard]] constexpr Assignment assignment(std::string_view line)
{
    const std::size_t equals = line.find('=');
    Assignment result{.name = trim(line.substr(0, equals)), .value = trim(line.substr(equals + 1))};
    if (result.value.size() >= 2 && (result.value.front() == '"' || result.value.front() == '\'') &&
        result.value.back() == result.value.front())
    {
        result.value = result.value.substr(1, result.value.size() - 2);
    }
    return result;
}

/// Whether @p word is one of cron's named schedules.
[[nodiscard]] constexpr bool isNamedSchedule(std::string_view word) noexcept
{
    constexpr std::array<std::string_view, 8> NAMES = {
        "@reboot",
        "@yearly",
        "@annually",
        "@monthly",
        "@weekly",
        "@daily",
        "@midnight",
        "@hourly",
    };
    return std::ranges::any_of(NAMES, [word](std::string_view name) { return word == name; });
}

} // namespace Detail

/// The job lines of a cron table. @p withUser: the system tables' format, with the user column after the schedule.
/// Comments, blank lines and variable assignments are skipped, and so is a line too short to be a job.
[[nodiscard]] inline std::vector<Entry> parse(std::string_view text, bool withUser)
{
    std::vector<Entry> entries;
    std::string path(DEFAULT_PATH);
    std::string shell(DEFAULT_SHELL);
    std::size_t lineNumber = 0;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
        {
            end = text.size();
        }
        ++lineNumber;
        const std::string_view line = Detail::trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line.front() == '#')
        {
            continue; // past the last line, start > text.size() ends the loop
        }
        if (Detail::isAssignment(line))
        {
            // cron sets each in the environment of the jobs below it; the job's shell and its PATH decide what runs.
            const Detail::Assignment set = Detail::assignment(line);
            if (set.name == "PATH")
            {
                path = std::string(set.value);
            }
            else if (set.name == "SHELL")
            {
                shell = std::string(set.value);
            }
            continue;
        }
        std::string_view rest = line;
        std::string schedule;
        const std::string_view first = Detail::nextWord(rest);
        if (first.starts_with('@'))
        {
            if (!Detail::isNamedSchedule(first))
            {
                continue;
            }
            schedule = std::string(first);
        }
        else
        {
            schedule = std::string(first);
            bool complete = true;
            for (int field = 1; field < 5; ++field)
            {
                const std::string_view word = Detail::nextWord(rest);
                if (word.empty())
                {
                    complete = false;
                    break;
                }
                schedule += ' ';
                schedule += word;
            }
            if (!complete)
            {
                continue;
            }
        }
        std::string user;
        if (withUser)
        {
            user = std::string(Detail::nextWord(rest));
        }
        const std::string_view command = Detail::trim(rest);
        if (command.empty() || (withUser && user.empty()))
        {
            continue;
        }
        entries.push_back(Entry{
            .schedule = std::move(schedule),
            .user = std::move(user),
            .command = std::string(command),
            .line = lineNumber,
            .path = path,
            .shell = shell,
        });
    }
    return entries;
}

/// What a job's command starts, read as its shell would read the first command, without running anything.
struct Program
{
    enum class Kind : std::uint8_t
    {
        Executable,   ///< word is the program, as written (a path, or a name looked up on PATH)
        ShellBuiltin, ///< word is a builtin or keyword the shell runs itself (cd, test, if, ...): no program file
        Unknown,      ///< shell syntax this doesn't follow (an expansion, a redirection or subshell first, ...)
    };
    Kind kind = Kind::Unknown;
    std::string word;                ///< The first command word, unquoted; empty when Unknown
    std::optional<std::string> path; ///< A PATH set for it on the line itself (`PATH=/opt/x cmd`, `env PATH=...`)
    /// `env -i` or `env -u PATH` removed the PATH it inherits and none was set after: env's execvp() then searches
    /// its default, /bin:/usr/bin, not the table's PATH.
    bool pathRemoved = false;
};

namespace Detail
{

/// Characters that end a shell word and start an operator or redirection.
[[nodiscard]] constexpr bool isShellOperator(char c) noexcept
{
    return c == ';' || c == '&' || c == '|' || c == '<' || c == '>' || c == '(' || c == ')' || c == '\n';
}

/// The command as cron hands it to the shell: up to the first unescaped '%' (the rest is its standard input), with
/// "\%" made a plain '%' (crontab(5)).
[[nodiscard]] inline std::string shellCommand(std::string_view command)
{
    std::string text;
    std::size_t i = 0;
    while (i < command.size())
    {
        const char c = command[i];
        if (c == '\\' && i + 1 < command.size() && command[i + 1] == '%')
        {
            text += '%';
            i += 2;
            continue;
        }
        if (c == '%')
        {
            break;
        }
        text += c;
        ++i;
    }
    return text;
}

/// One shell word read from the front of @p text: quotes removed ('...' literal; "..." and \ escaping as sh does).
/// nullopt for anything whose value only the shell could work out: a $ or ` expansion, a glob, a leading ~ or #,
/// or an unterminated quote. assignmentAt: where an unquoted NAME= ends its name, else npos.
struct ShellWord
{
    std::string text;
    std::size_t assignmentAt = std::string::npos;
};

[[nodiscard]] inline std::optional<ShellWord> readShellWord(std::string_view& text)
{
    ShellWord word;
    bool quoted = false;
    std::size_t i = 0;
    while (i < text.size())
    {
        const char c = text[i];
        if (isBlank(c) || isShellOperator(c))
        {
            break;
        }
        if (c == '\'')
        {
            const std::size_t close = text.find('\'', i + 1);
            if (close == std::string_view::npos)
            {
                return std::nullopt;
            }
            word.text.append(text.substr(i + 1, close - i - 1));
            i = close + 1;
            quoted = true;
            continue;
        }
        if (c == '"')
        {
            std::size_t j = i + 1;
            while (j < text.size() && text[j] != '"')
            {
                const char d = text[j];
                if (d == '$' || d == '`')
                {
                    return std::nullopt;
                }
                if (d == '\\' && j + 1 < text.size() &&
                    (text[j + 1] == '$' || text[j + 1] == '`' || text[j + 1] == '"' || text[j + 1] == '\\' || text[j + 1] == '\n'))
                {
                    if (text[j + 1] != '\n')
                    {
                        word.text += text[j + 1];
                    }
                    j += 2;
                    continue;
                }
                word.text += d;
                ++j;
            }
            if (j >= text.size())
            {
                return std::nullopt;
            }
            i = j + 1; // past the closing quote
            quoted = true;
            continue;
        }
        if (c == '\\')
        {
            if (i + 1 >= text.size())
            {
                return std::nullopt;
            }
            if (text[i + 1] != '\n')
            {
                word.text += text[i + 1];
            }
            i += 2;
            quoted = true;
            continue;
        }
        if (c == '[')
        {
            // A glob only with a ']' later in the word; "[" and "[[" on their own are the test commands.
            std::size_t end = i + 1;
            while (end < text.size() && !isBlank(text[end]) && !isShellOperator(text[end]) && text[end] != ']')
            {
                ++end;
            }
            if (end < text.size() && text[end] == ']')
            {
                return std::nullopt;
            }
        }
        if (c == '$' || c == '`' || c == '*' || c == '?' || ((c == '~' || c == '#') && i == 0))
        {
            return std::nullopt;
        }
        if (c == '=' && !quoted && word.assignmentAt == std::string::npos)
        {
            word.assignmentAt = word.text.size();
        }
        word.text += c;
        ++i;
    }
    text.remove_prefix(i);
    return word;
}

/// Whether @p word, the first word of a command, is one the shell runs itself rather than a program it looks up:
/// POSIX sh's reserved words and builtins, and the common ones bash and dash add.
[[nodiscard]] constexpr bool isShellBuiltin(std::string_view word) noexcept
{
    constexpr auto BUILTINS = std::to_array<std::string_view>({
        "!",       "{",        "}",    "[",       "[[",       "case",    "do",     "done",    "elif",   "else",     "esac",   "fi",
        "for",     "function", "if",   "in",      "select",   "then",    "until",  "while",   ".",      ":",        "alias",  "bg",
        "break",   "builtin",  "cd",   "command", "continue", "declare", "echo",   "eval",    "exit",   "export",   "false",  "fg",
        "getopts", "hash",     "jobs", "kill",    "let",      "local",   "printf", "pwd",     "read",   "readonly", "return", "set",
        "shift",   "source",   "test", "times",   "trap",     "true",    "type",   "typeset", "ulimit", "umask",    "unset",  "wait",
    });
    return std::ranges::any_of(BUILTINS, [word](std::string_view name) { return word == name; });
}

/// Whether @p name is a shell variable name.
[[nodiscard]] constexpr bool isShellName(std::string_view name) noexcept
{
    if (name.empty() || (name.front() >= '0' && name.front() <= '9'))
    {
        return false;
    }
    return std::ranges::all_of(
        name, [](char c) { return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

} // namespace Detail

/// The program @p command (an Entry's) starts, read as a POSIX shell would read its first simple command, but never
/// run: quotes and backslashes removed, VAR=value prefixes skipped (a PATH= one kept), and `env` (with its options
/// and assignments) or `exec` before it skipped. Its first word may be a shell builtin or keyword (`cd / && ...`);
/// anything the shell would have to expand or that starts with a redirection, a subshell or a pipeline is Unknown.
[[nodiscard]] inline Program program(std::string_view command)
{
    const std::string line = Detail::shellCommand(command);
    std::string_view rest = line;
    Program result;
    bool afterEnv = false;
    bool afterExec = false;
    // env's -u NAME (its name next) or -C DIR (skipped) left waiting for its argument.
    enum class EnvArgument : std::uint8_t
    {
        None,
        UnsetName,
        Skipped,
    };
    EnvArgument pending = EnvArgument::None;
    const auto unset = [&result](std::string_view name)
    {
        if (name == "PATH")
        {
            result.path.reset();
            result.pathRemoved = true;
        }
    };
    for (;;)
    {
        while (!rest.empty() && Detail::isBlank(rest.front()))
        {
            rest.remove_prefix(1);
        }
        std::optional<Detail::ShellWord> word = Detail::readShellWord(rest);
        if (!word.has_value() || word->text.empty())
        {
            return Program{}; // an expansion, an operator first ("(cd /tmp)", "> log cmd"), or nothing at all
        }
        if (!rest.empty() && (rest.front() == '<' || rest.front() == '>') &&
            std::ranges::all_of(word->text, [](char c) { return c >= '0' && c <= '9'; }))
        {
            return Program{}; // "2>/dev/null cmd": a redirection first
        }
        if (pending != EnvArgument::None)
        {
            if (pending == EnvArgument::UnsetName)
            {
                unset(word->text);
            }
            pending = EnvArgument::None;
            continue;
        }
        if (word->assignmentAt != std::string::npos && Detail::isShellName(std::string_view(word->text).substr(0, word->assignmentAt)))
        {
            if (std::string_view(word->text).substr(0, word->assignmentAt) == "PATH")
            {
                result.path = word->text.substr(word->assignmentAt + 1);
                result.pathRemoved = false;
            }
            continue;
        }
        if (afterEnv && word->text.starts_with('-'))
        {
            const std::string_view option = word->text;
            if (option.starts_with("--split-string") || option.starts_with("--default-signal"))
            {
                return Program{}; // env splits its own string, or changes signals with an optional argument: not followed
            }
            if (option == "-" || option == "--ignore-environment")
            {
                unset("PATH"); // every inherited variable, PATH among them
            }
            else if (option == "--unset" || option == "--chdir")
            {
                pending = option == "--unset" ? EnvArgument::UnsetName : EnvArgument::Skipped;
            }
            else if (option.starts_with("--unset="))
            {
                unset(option.substr(std::string_view("--unset=").size()));
            }
            else if (!option.starts_with("--"))
            {
                // A cluster of short options (-i, -0, -v), where -u and -C take the rest of it or the next word.
                for (std::size_t at = 1; at < option.size(); ++at)
                {
                    const char flag = option[at];
                    if (flag == 'S' || flag == 'P')
                    {
                        return Program{}; // env splits its own string, or searches another path: not followed
                    }
                    if (flag == 'i')
                    {
                        unset("PATH");
                    }
                    else if (flag == 'u' || flag == 'C')
                    {
                        const std::string_view argument = option.substr(at + 1);
                        if (argument.empty())
                        {
                            pending = flag == 'u' ? EnvArgument::UnsetName : EnvArgument::Skipped;
                        }
                        else if (flag == 'u')
                        {
                            unset(argument);
                        }
                        break;
                    }
                }
            }
            continue;
        }
        if (afterExec && word->text.starts_with('-'))
        {
            return Program{}; // bash's exec -a NAME / -c / -l
        }
        if (!afterEnv && (word->text == "env" || word->text.ends_with("/env")))
        {
            afterEnv = true;
            continue;
        }
        if (!afterEnv && !afterExec && word->text == "exec")
        {
            afterExec = true;
            continue;
        }
        result.kind = !afterEnv && Detail::isShellBuiltin(word->text) ? Program::Kind::ShellBuiltin : Program::Kind::Executable;
        result.word = std::move(word->text);
        return result;
    }
}

/// How the Startup tab words @p schedule: "at startup" for @reboot, else as written ("@daily", "*/5 * * * *").
[[nodiscard]] inline std::string describeSchedule(std::string_view schedule)
{
    return schedule == "@reboot" ? std::string("at startup") : std::string(schedule);
}

} // namespace Platform::CronTab
