#pragma once

// One walk of a process's /proc/[pid]/fd directory, shared by the FD count LinuxProcessProbe
// reports and the socket inode-to-PID map network attribution needs (#1426). Both used to list
// every process's fd directory separately; LinuxProcessProbe::enumerate() now walks it once per
// sample and, on a sample when the map is due for its rebuild, reads every link as it counts.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(__linux__) && __has_include(<dirent.h>) && __has_include(<unistd.h>)
#include "PosixGuards.h"

#include <array>
#include <cerrno>
#include <concepts>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Platform::ProcFdScan
{

/// The socket inode a /proc/[pid]/fd link target names ("socket:[12345]"), or nullopt for any other
/// target (a file, a pipe, an anon inode) or a malformed one.
[[nodiscard]] constexpr std::optional<std::uint64_t> socketInode(std::string_view target) noexcept
{
    constexpr std::string_view PREFIX = "socket:[";
    if (!target.starts_with(PREFIX))
    {
        return std::nullopt;
    }
    target.remove_prefix(PREFIX.size());
    const std::size_t close = target.find(']');
    if (close == std::string_view::npos)
    {
        return std::nullopt;
    }
    std::uint64_t inode = 0;
    std::string_view digits = target;
    digits.remove_suffix(digits.size() - close); // not substr(): it may throw, and this is noexcept
    const auto [end, ec] = std::from_chars(digits.begin(), digits.end(), inode);
    if (ec != std::errc{} || end != digits.end() || inode == 0)
    {
        return std::nullopt;
    }
    return inode;
}

#if defined(__linux__) && __has_include(<dirent.h>) && __has_include(<unistd.h>)

/// Whether a process's fd links can be read, judged on the first entry that answers. Listing the
/// directory needs only DAC permission (CAP_DAC_READ_SEARCH for another user's process), but reading
/// its links -- which the socket inode-to-PID map does -- also needs ptrace access (CAP_SYS_PTRACE).
enum class LinkAccess : std::uint8_t
{
    Unknown, // No entry answered (none, or every one closed between the listing and its readlink)
    Readable,
    Denied, // EACCES/EPERM: the process's connections can't be attributed to it (#1328)
};

/// One walk of a process's fd directory.
struct FdScan
{
    bool listed = false;    // Opened and listed to the end; otherwise the count is unknown, not 0 (#1110)
    std::int32_t count = 0; // Entries listed (open file descriptors), when `listed`
    LinkAccess linkAccess = LinkAccess::Unknown;
};

/// List the fd directory of the /proc/[pid] directory `pidDirFd` is open on, counting its entries.
/// Reading through that handle keeps the walk on one process: once it exits, the reads fail rather
/// than reach a process that reused its PID (#1336).
///
/// Only the first entry's link is read (to judge LinkAccess) unless `readEveryLink`, which reads every
/// link and calls `onSocket(inode)` for each one that is a socket -- the inode-to-PID map's input.
/// Dots aside, every entry counts: an fd that closes between the listing and its readlink was open
/// when listed.
template<std::invocable<std::uint64_t> OnSocket> [[nodiscard]] FdScan scanFds(int pidDirFd, bool readEveryLink, OnSocket onSocket)
{
    FdScan scan;
    // An FdGuard, so the descriptor can't leak if onSocket throws (e.g. std::bad_alloc on a map
    // rehash, #772).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const Posix::FdGuard fdDir(::openat(pidDirFd, "fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fdDir.get() == -1)
    {
        return scan; // Permission denied (another user's process), or the process exited
    }

    // Listed with getdents64() into a stack buffer rather than opendir()/readdir(): this runs for
    // every process on every sample, and a DIR* costs a 32 KiB allocation plus an fstat() and an
    // fcntl() each time. A buffer of this size lists a few hundred fds per call.
    constexpr std::size_t LISTING_BUFFER_SIZE = 8192;
    alignas(dirent64) std::array<char, LISTING_BUFFER_SIZE> listing{};
    // "socket:[" plus a 20-digit inode and "]" fits with room to spare; a longer target (a file path)
    // comes back cut off, which is still no socket.
    std::array<char, 256> linkTarget{};
    for (;;)
    {
        const ssize_t listed = ::getdents64(fdDir.get(), listing.data(), listing.size());
        if (listed < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return scan; // An error part-way through the listing: the count is unknown
        }
        if (listed == 0)
        {
            scan.listed = true; // The end of the listing
            return scan;
        }
        // Each record: a linux_dirent64 header, then its NUL-terminated name, d_reclen bytes in all.
        // The fields are copied out rather than read through a cast pointer.
        for (std::size_t offset = 0; std::cmp_less(offset, listed);)
        {
            unsigned short recordLength = 0;
            std::memcpy(&recordLength, listing.data() + offset + offsetof(dirent64, d_reclen), sizeof(recordLength));
            const char* const nameStart = listing.data() + offset + offsetof(dirent64, d_name);
            offset += recordLength;
            if (recordLength == 0)
            {
                return scan; // A malformed record: not a listing to trust
            }

            const std::string_view name(nameStart);
            if (name == "." || name == "..")
            {
                continue;
            }
            ++scan.count;
            if (!readEveryLink && scan.linkAccess != LinkAccess::Unknown)
            {
                continue;
            }

            const ssize_t linkLen = ::readlinkat(fdDir.get(), nameStart, linkTarget.data(), linkTarget.size());
            if (linkLen >= 0)
            {
                if (scan.linkAccess == LinkAccess::Unknown)
                {
                    scan.linkAccess = LinkAccess::Readable;
                }
                if (readEveryLink)
                {
                    if (const auto inode = socketInode(std::string_view(linkTarget.data(), static_cast<std::size_t>(linkLen))))
                    {
                        onSocket(*inode);
                    }
                }
            }
            else if (scan.linkAccess == LinkAccess::Unknown)
            {
                if (errno == EINVAL)
                {
                    scan.linkAccess = LinkAccess::Readable; // Not a link (a synthetic /proc), but access was granted
                }
                else if (errno == EACCES || errno == EPERM)
                {
                    scan.linkAccess = LinkAccess::Denied;
                }
                // Anything else (ENOENT: the fd closed since the listing): judge on the next entry.
            }
        }
    }
}

#endif

} // namespace Platform::ProcFdScan
