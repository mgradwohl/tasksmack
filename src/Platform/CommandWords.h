#pragma once

// Run new task (#3065): a typed command line split into the program and its arguments the way a POSIX
// shell splits words, without running a shell, so nothing is expanded ($VAR, globs, ~) and nothing but
// the program named runs. Platform is the lowest layer, so App code can include it too.

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

} // namespace Platform::CommandWords
