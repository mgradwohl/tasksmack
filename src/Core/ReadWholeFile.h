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
//
// On Linux the file is opened with the user's own permissions (#4802): a capabilities session's CAP_DAC_READ_SEARCH is
// lowered for the open (Platform::UserPermissionsOnly), so a settings folder or theme the environment names can't lead
// it into another user's files. Every caller reads the user's settings or themes, which the user may read anyway.
//
// On Windows the file is opened sharing read and write (_SH_DENYNO), as the std::ifstream before #4652 did (#4745):
// _wfopen_s opens it unshared (_SH_SECURE: read sharing only), so an editor or a sync tool holding config.toml or a
// theme open for writing made the read fail, and the load fell back to defaults, while the other program's own open
// for writing failed for as long as the read took.

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

#ifdef _WIN32
#include <cstdio>
#include <memory>

#include <share.h>
#else
#ifdef __linux__
#include "Platform/Linux/UserPermissionsOnly.h"
#endif

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace Core
{

#ifndef _WIN32
namespace Detail
{
/// Closes the descriptor it holds when it dies: on every way out of readWholeFile(), a throwing append included (#4746).
class ClosesFd
{
  public:
    explicit ClosesFd(int fd) noexcept : m_Fd(fd)
    {}
    ~ClosesFd()
    {
        ::close(m_Fd);
    }
    ClosesFd(const ClosesFd&) = delete;
    ClosesFd& operator=(const ClosesFd&) = delete;
    ClosesFd(ClosesFd&&) = delete;
    ClosesFd& operator=(ClosesFd&&) = delete;

  private:
    int m_Fd;
};
} // namespace Detail
#endif

/// The bytes of the file at @p path, opened by the path itself, never by a narrowed name. nullopt when it can't be
/// opened, or a read fails before its end (EIO, ESTALE, EISDIR for a folder): never a part of it as if it were all.
[[nodiscard]] inline std::optional<std::string> readWholeFile(const std::filesystem::path& path)
{
    std::string text;
    std::array<char, 16384> chunk{};
#ifdef _WIN32
    std::FILE* const opened = ::_wfsopen(path.c_str(), L"rb", _SH_DENYNO); // shared, see above (#4745)
    if (opened == nullptr)
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
    int fd = -1;
    {
#ifdef __linux__
        // The user's own permissions for the open (#4802); an open descriptor reads on without them.
        const Platform::UserPermissionsOnly userPermissions;
        if (!userPermissions.ok())
        {
            return std::nullopt;
        }
#endif
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0)
    {
        return std::nullopt;
    }
    const Detail::ClosesFd closesFd(fd); // closed on every way out
    for (;;)
    {
        // A caller may hold its own lock over the read of a small local file (UserConfig's config.toml, a theme), as it
        // did over the std::ifstream read before #4652; the UI thread never waits on such a lock (#2916).
        // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection) - as above
        const auto got = ::read(fd, chunk.data(), chunk.size());
        if (got < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return std::nullopt;
        }
        if (got == 0)
        {
            break;
        }
        text.append(chunk.data(), static_cast<std::size_t>(got));
    }
#endif
    return text;
}

} // namespace Core
