#pragma once

// A whole file read by its path (#4315): the config and the themes are read with this, then parsed from the text, so
// the file opened is the one the path names. A UTF-8 file name handed to a library (toml++'s parse_file) is opened
// through that text, and on Windows a path whose name holds a lone surrogate (NTFS allows one) has no UTF-8 form:
// Core::pathToUtf8() shows it as U+FFFD, which names another file, or none. std::ifstream takes the path itself,
// its wide native form on Windows, so nothing is narrowed on the way.

#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>

namespace Core
{

/// The bytes of the file at @p path, opened by the path itself, never by a narrowed name. nullopt when it can't be
/// opened or read through.
[[nodiscard]] inline std::optional<std::string> readWholeFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return std::nullopt;
    }
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad())
    {
        return std::nullopt;
    }
    return text;
}

} // namespace Core
