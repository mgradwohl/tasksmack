#pragma once

// Run new task (#3065): a typed command line split into the program and its arguments the way a POSIX
// shell splits words, without running a shell, so nothing is expanded ($VAR, globs, ~) and nothing but
// the program named runs. Windows (#3427) splits it as the Run box does instead: the program (a
// document, a folder or a web address too), then its parameters as typed, which the program parses
// itself (splitProgram(), quoteProgram()). Platform is the lowest layer, so App code can include it too.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::CommandWords
{

/// @p commandLine's words: split on spaces and tabs, with '...' taken literally, "..." keeping its
/// spaces (a backslash there escapes only " \ $ ` and a newline), and a backslash outside quotes
/// escaping the next character. An unterminated quote, or a backslash at the very end, is an error.
/// No words (blank) is an empty list.
[[nodiscard]] inline std::expected<std::vector<std::string>, std::string> split(std::string_view commandLine)
{
    std::vector<std::string> words;
    std::string word;
    bool inWord = false;
    enum class Quote : std::uint8_t
    {
        None,
        Single,
        Double,
    };
    Quote quote = Quote::None;
    // A while loop, not a for: an escape consumes the next character too, so i advances in the body.
    std::size_t i = 0;
    while (i < commandLine.size())
    {
        const char c = commandLine[i++];
        if (quote == Quote::Single)
        {
            if (c == '\'')
            {
                quote = Quote::None;
            }
            else
            {
                word.push_back(c);
            }
            continue;
        }
        if (quote == Quote::Double)
        {
            if (c == '"')
            {
                quote = Quote::None;
            }
            else if (c == '\\' && i < commandLine.size() && std::string_view("\"\\$`\n").contains(commandLine[i]))
            {
                word.push_back(commandLine[i++]);
            }
            else
            {
                word.push_back(c);
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n')
        {
            if (inWord)
            {
                words.push_back(std::move(word));
                word.clear();
                inWord = false;
            }
            continue;
        }
        inWord = true;
        if (c == '\'')
        {
            quote = Quote::Single;
        }
        else if (c == '"')
        {
            quote = Quote::Double;
        }
        else if (c == '\\')
        {
            if (i >= commandLine.size())
            {
                return std::unexpected(std::string("The command ends with a lone backslash"));
            }
            word.push_back(commandLine[i++]);
        }
        else
        {
            word.push_back(c);
        }
    }
    if (quote != Quote::None)
    {
        return std::unexpected(std::string(quote == Quote::Single ? "A ' quote isn't closed" : "A \" quote isn't closed"));
    }
    if (inWord)
    {
        words.push_back(std::move(word));
    }
    return words;
}

/// @p path as one word for a command line: as it is when it needs no quoting, else in '...' with any '
/// written as '\'' (Browse... puts the chosen file in the command line this way).
[[nodiscard]] inline std::string quote(std::string_view path)
{
    if (!path.empty() && path.find_first_of(" \t\n'\"\\$`*?[]{}()<>|&;#~") == std::string_view::npos)
    {
        return std::string(path);
    }
    std::string quoted = "'";
    for (const char c : path)
    {
        if (c == '\'')
        {
            quoted += "'\\''";
        }
        else
        {
            quoted.push_back(c);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

/// A Windows command line's program and parameters (#3427), as splitProgram() splits it.
struct ProgramAndParameters
{
    std::string program;    ///< What to start: a program, a document, a folder or a web address
    std::string parameters; ///< The rest, as typed, for the program to parse; empty when there is none
};

/// @p commandLine split as Windows' Run box splits it (#3427): leading and trailing spaces and tabs
/// dropped; then the program, which is the text inside a leading "...", or else everything up to the
/// first space or tab; then the parameters, everything after it with the spaces before it dropped,
/// untouched, since each Windows program parses its own command line. A leading " left open, or a
/// blank or empty "" program, is an error. UTF-8 is split on its ASCII bytes only, so any character
/// stays whole.
[[nodiscard]] inline std::expected<ProgramAndParameters, std::string> splitProgram(std::string_view commandLine)
{
    constexpr std::string_view BLANKS = " \t";
    const std::size_t first = commandLine.find_first_not_of(BLANKS);
    if (first == std::string_view::npos)
    {
        return std::unexpected(std::string("Type the program to run"));
    }
    std::string_view rest = commandLine.substr(first, commandLine.find_last_not_of(BLANKS) + 1 - first);
    ProgramAndParameters split;
    if (rest.front() == '"')
    {
        const std::size_t close = rest.find('"', 1);
        if (close == std::string_view::npos)
        {
            return std::unexpected(std::string("A \" quote isn't closed"));
        }
        split.program = std::string(rest.substr(1, close - 1));
        rest.remove_prefix(close + 1);
        if (split.program.find_first_not_of(BLANKS) == std::string::npos)
        {
            return std::unexpected(std::string("Type the program to run inside the quotes"));
        }
    }
    else
    {
        const std::size_t end = std::min(rest.find_first_of(BLANKS), rest.size());
        split.program = std::string(rest.substr(0, end));
        rest.remove_prefix(end);
    }
    const std::size_t parameters = rest.find_first_not_of(BLANKS);
    if (parameters != std::string_view::npos)
    {
        split.parameters = std::string(rest.substr(parameters));
    }
    return split;
}

/// @p path as the program of a Windows command line (#3427): in "..." when it is empty or holds a space or
/// tab, else as it is (Browse... puts the chosen file in the command line this way). Windows paths can't
/// hold a '"', so none needs escaping.
[[nodiscard]] inline std::string quoteProgram(std::string_view path)
{
    if (!path.empty() && path.find_first_of(" \t") == std::string_view::npos)
    {
        return std::string(path);
    }
    std::string quoted = "\"";
    quoted += path;
    quoted.push_back('"');
    return quoted;
}

} // namespace Platform::CommandWords
