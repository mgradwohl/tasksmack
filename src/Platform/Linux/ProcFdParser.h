#pragma once

// /proc/[pid]/fd and /proc/[pid]/fdinfo as open files (#183): pure functions, free of any system
// call, so they are unit tested and fuzzed on every platform (tests/Platform/test_ProcFdParser.cpp,
// tests/fuzz/fuzz_proc_fd.cpp).
//
// readlink() on /proc/[pid]/fd/<n> gives the target's path, with " (deleted)" appended when it was
// unlinked, or the kernel's name for an object with no path: "socket:[1234]", "pipe:[5678]",
// "anon_inode:[eventfd]", "anon_inode:inotify", "net:[4026531840]". A memfd reads as
// "/memfd:name (deleted)". fdinfo/<n> is "pos:\t0\nflags:\t02100002\nmnt_id:\t..." (flags in octal).
//
// The type is told from the text alone, never by stat()ing the target (which can block on a dead
// network mount, as lsof can): a path under /dev is a device (but not /dev/shm or /dev/mqueue,
// which hold files), and a descriptor opened with O_DIRECTORY (opendir() and friends) is a directory.

#include "Platform/IProcessOpenFiles.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Platform::ProcFd
{

/// Linux's O_DIRECTORY as the kernel reports it in fdinfo (not the host's <fcntl.h>: this is compiled
/// and tested on Windows too). The access-mode bits the UI shows are Domain's (Domain/ProcessOpenFiles.h).
#if defined(__arm__) || defined(__aarch64__)
inline constexpr std::uint32_t DIRECTORY = 040000;
#else
inline constexpr std::uint32_t DIRECTORY = 0200000;
#endif

inline constexpr std::string_view DELETED_SUFFIX = " (deleted)";

/// @p link (a readlink() of /proc/[pid]/fd/<n>) as an open file of @p descriptor, typed. @p flags are
/// the descriptor's fdinfo flags, if read (they tell a directory).
[[nodiscard]] inline OpenFile classifyFdLink(std::uint64_t descriptor, std::string_view link, std::optional<std::uint32_t> flags)
{
    OpenFile file{.descriptor = descriptor, .kind = OpenFileKind::Other, .path = {}, .flags = flags, .deleted = false};
    const auto startsWith = [link](std::string_view prefix) noexcept
    {
        return link.starts_with(prefix);
    };
    if (startsWith("socket:["))
    {
        file.kind = OpenFileKind::Socket;
    }
    else if (startsWith("pipe:["))
    {
        file.kind = OpenFileKind::Pipe;
    }
    else if (startsWith("anon_inode:"))
    {
        file.kind = OpenFileKind::AnonInode;
    }
    else if (startsWith("/memfd:"))
    {
        // Always "(deleted)": a memfd never had a path. Shown as "memfd:name", not as a deleted file.
        file.kind = OpenFileKind::Memfd;
        link.remove_prefix(1);
        if (link.ends_with(DELETED_SUFFIX))
        {
            link.remove_suffix(DELETED_SUFFIX.size());
        }
    }
    else if (startsWith("/"))
    {
        if (link.ends_with(DELETED_SUFFIX))
        {
            file.deleted = true;
            link.remove_suffix(DELETED_SUFFIX.size());
        }
        if (flags.has_value() && (*flags & DIRECTORY) != 0)
        {
            file.kind = OpenFileKind::Directory;
        }
        else if (startsWith("/dev/") && !startsWith("/dev/shm/") && !startsWith("/dev/mqueue/"))
        {
            file.kind = OpenFileKind::Device;
        }
        else
        {
            file.kind = OpenFileKind::File;
        }
    }
    file.path = std::string(link);
    return file;
}

/// The "flags:" value (octal) in @p fdinfo, the text of /proc/[pid]/fdinfo/<n>; nullopt when it has
/// none or it does not parse.
[[nodiscard]] inline std::optional<std::uint32_t> parseFdInfoFlags(std::string_view fdinfo) noexcept
{
    constexpr std::string_view KEY = "flags:";
    while (!fdinfo.empty())
    {
        const std::size_t newline = fdinfo.find('\n');
        const std::size_t lineLength = newline == std::string_view::npos ? fdinfo.size() : newline;
        std::string_view line = fdinfo;
        line.remove_suffix(line.size() - lineLength); // not substr(): it may throw, and this is noexcept
        fdinfo.remove_prefix(newline == std::string_view::npos ? fdinfo.size() : newline + 1);
        if (!line.starts_with(KEY))
        {
            continue;
        }
        line.remove_prefix(KEY.size());
        const std::size_t digits = line.find_first_not_of(" \t");
        if (digits == std::string_view::npos)
        {
            return std::nullopt;
        }
        line.remove_prefix(digits);
        std::uint32_t flags = 0;
        const auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), flags, 8);
        if (ec != std::errc{} || ptr == line.data())
        {
            return std::nullopt;
        }
        return flags;
    }
    return std::nullopt;
}

/// The descriptor number an entry of /proc/[pid]/fd is named for; nullopt for "." and "..".
[[nodiscard]] inline std::optional<std::uint64_t> parseFdName(std::string_view name) noexcept
{
    std::uint64_t fd = 0;
    const auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), fd, 10);
    if (ec != std::errc{} || name.empty() || ptr != name.data() + name.size())
    {
        return std::nullopt;
    }
    return fd;
}

} // namespace Platform::ProcFd
