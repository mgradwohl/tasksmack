#pragma once

// A whole file read by its path (#4315): the config and the themes are read with this, then parsed from the text, so
// the file opened is the one the path names. A UTF-8 file name handed to a library (toml++'s parse_file) is opened
// through that text, and on Windows a path whose name holds a lone surrogate (NTFS allows one) has no UTF-8 form:
// Core::pathToUtf8() shows it as U+FFFD, which names another file, or none. The path itself is opened, its wide
// native form on Windows, so nothing is narrowed on the way.
//
// A read that fails part-way is a failure, not the end of the file (#4652): std::ifstream's buffer turns a failed
// read into EOF on libc++ and the MSVC STL, so a config.toml on a failing disk, or an NFS home returning EIO or
// ESTALE, came back cut short or empty as if whole, and the exit save then rewrote the user's file from it (#1122's
// "a file that can't be read is left alone"). So it reads with read(2) on POSIX and fread() with ferror() on Windows,
// as Sha256::hashFile does since #4422.

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

#ifdef _WIN32
#include <cstdio>
#include <memory>
#else
#include <cerrno>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace Core
{

/// The bytes of the file at @p path, opened by the path itself, never by a narrowed name. nullopt when it can't be
/// opened, or a read fails before its end (EIO, ESTALE, EISDIR for a folder): never a part of it as if it were all.
[[nodiscard]] inline std::optional<std::string> readWholeFile(const std::filesystem::path& path)
{
    std::string text;
    std::array<char, 16384> chunk{};
#ifdef _WIN32
    std::FILE* opened = nullptr;
    if (::_wfopen_s(&opened, path.c_str(), L"rb") != 0 || opened == nullptr)
    {
        return std::nullopt;
    }
    const std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(opened, &std::fclose); // closed on every way out
    for (;;)
    {
        const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), file.get());
        text.append(chunk.data(), got);
        if (got < chunk.size())
        {
            break; // the end, or an error: ferror() tells them apart
        }
    }
    if (std::ferror(file.get()) != 0)
    {
        return std::nullopt;
    }
#else
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return std::nullopt;
    }
    for (;;)
    {
        const auto got = ::read(fd, chunk.data(), chunk.size());
        if (got < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            ::close(fd);
            return std::nullopt;
        }
        if (got == 0)
        {
            break;
        }
        text.append(chunk.data(), static_cast<std::size_t>(got));
    }
    ::close(fd);
#endif
    return text;
}

} // namespace Core
