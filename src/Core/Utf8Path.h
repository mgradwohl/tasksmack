#pragma once

// std::filesystem::path <-> UTF-8 text, independent of the process's ANSI code page (#1648).
//
// On Windows a path holds UTF-16. path::string() and path(std::string) convert through the active
// code page: UTF-8 in TaskSmack.exe (its manifest sets activeCodePage), but the system's code page
// (often 1252) in every binary without that manifest -- the tests, the UI training driver, the
// benchmarks. There a narrow conversion of a CJK or emoji path throws, and an accented one comes out
// as code-page bytes that a UTF-8 consumer (toml++, ImGui, SDL, spdlog) misreads. These two helpers
// never go through the code page, so they are correct with or without the manifest. On Linux a path
// is bytes and both are plain copies, so a file name that isn't valid UTF-8 still round-trips.
//
// Use them wherever a path meets UTF-8 text: a log line, a library that takes a UTF-8 file name
// (toml++'s parse_file, stb_image with STBI_WINDOWS_UTF8, SDL), or a path read from UTF-8 input.
// To open a file, pass the path itself (std::ifstream, std::filesystem) or path.wstring() to a W API.

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace Core
{

/// @p path as UTF-8. Never throws for an encoding reason: on Windows an unpaired UTF-16 surrogate
/// (NTFS allows one in a file name) becomes U+FFFD, so the text is for display and UTF-8 APIs, not
/// for rebuilding that exact path. On Linux the native bytes, unchanged.
[[nodiscard]] inline std::string pathToUtf8(const std::filesystem::path& path)
{
#ifdef _WIN32
    constexpr wchar_t REPLACEMENT = 0xFFFD;
    const std::wstring& native = path.native();
    std::wstring wide;
    wide.reserve(native.size());
    std::size_t index = 0;
    while (index < native.size())
    {
        const wchar_t unit = native[index];
        const bool isHigh = unit >= 0xD800 && unit <= 0xDBFF;
        const bool isLow = unit >= 0xDC00 && unit <= 0xDFFF;
        const bool pairFollows = isHigh && index + 1 < native.size() && native[index + 1] >= 0xDC00 && native[index + 1] <= 0xDFFF;
        if (pairFollows)
        {
            wide.push_back(unit);
            wide.push_back(native[index + 1]);
            index += 2;
            continue;
        }
        wide.push_back((isHigh || isLow) ? REPLACEMENT : unit);
        ++index;
    }
    const std::u8string text = std::filesystem::path(std::move(wide)).u8string();
    return {text.begin(), text.end()};
#else
    return path.native();
#endif
}

/// The path @p utf8 names. On Windows the text is decoded as UTF-8, whatever the code page, and
/// text that isn't valid UTF-8 throws std::system_error; on Linux the bytes are the path's bytes.
[[nodiscard]] inline std::filesystem::path utf8ToPath(std::string_view utf8)
{
    return {std::u8string(utf8.begin(), utf8.end())};
}

} // namespace Core
