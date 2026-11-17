#pragma once

/// @file NewFileOwner.h
/// @brief Who should own the files and directories TaskSmack creates under the user's config directory
/// (#2121, #2437). Running as root with another user's HOME (sudo -E, as the elevation notice suggests),
/// anything created there would be root's, so the user's next normal run could neither read nor replace
/// it. UserConfig::save() and InstanceLock hand what they create to that user. POSIX only.

#ifndef _WIN32

#include <cerrno>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace App::NewFileOwner
{

/// A file's or directory's owner.
struct FileOwner
{
    uid_t uid;
    gid_t gid;
};

/// The owner of the nearest existing ancestor of @p dir (the user's ~/.config or home) when running as
/// root (@p effectiveUid 0); otherwise nullopt (not root, or root in root's own home): created as the
/// process, as usual.
[[nodiscard]] inline std::optional<FileOwner> ownerForNewFiles(const std::filesystem::path& dir, uid_t effectiveUid)
{
    if (effectiveUid != 0)
    {
        return std::nullopt;
    }
    std::filesystem::path at = dir;
    struct stat existing = {};
    while (::stat(at.c_str(), &existing) != 0)
    {
        if (at == at.parent_path())
        {
            return std::nullopt;
        }
        at = at.parent_path();
    }
    if (existing.st_uid == 0)
    {
        return std::nullopt;
    }
    return FileOwner{.uid = existing.st_uid, .gid = existing.st_gid};
}

/// Gives the directory @p dir, which the caller just created, to @p owner: through a descriptor opened
/// with O_NOFOLLOW, so a symlink swapped in for it can't make root hand some other directory over.
[[nodiscard]] inline bool chownCreatedDirectory(const std::filesystem::path& dir, const FileOwner& owner)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    const bool ok = ::fchown(fd, owner.uid, owner.gid) == 0;
    ::close(fd);
    return ok;
}

/// The first link on the way to @p dir (it included, or the nearest of its ancestors that exist) that a user other than
/// root owns, or nullopt. Run as root for a user (#3759), such a link is theirs to point anywhere, so a write (the
/// directory's creation included) would go where they chose; a link root owns is the system's own (/home on some
/// systems) and is followed as usual. lstat, so a link is seen as itself.
[[nodiscard]] inline std::optional<std::filesystem::path> userOwnedLinkOnPath(const std::filesystem::path& dir)
{
    for (std::filesystem::path at = dir; !at.empty(); at = at.parent_path())
    {
        struct stat entry = {};
        if (::lstat(at.c_str(), &entry) == 0 && S_ISLNK(entry.st_mode) && entry.st_uid != 0)
        {
            return at;
        }
        if (at == at.parent_path())
        {
            break;
        }
    }
    return std::nullopt;
}

/// A directory descriptor, closed with its owner.
class DirectoryFd
{
  public:
    DirectoryFd() noexcept = default;
    explicit DirectoryFd(int fd) noexcept : m_Fd(fd)
    {}
    DirectoryFd(const DirectoryFd&) = delete;
    DirectoryFd& operator=(const DirectoryFd&) = delete;
    DirectoryFd(DirectoryFd&& other) noexcept : m_Fd(std::exchange(other.m_Fd, -1))
    {}
    DirectoryFd& operator=(DirectoryFd&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_Fd = std::exchange(other.m_Fd, -1);
        }
        return *this;
    }
    ~DirectoryFd()
    {
        reset();
    }
    [[nodiscard]] int get() const noexcept
    {
        return m_Fd;
    }

  private:
    void reset() noexcept
    {
        if (m_Fd >= 0)
        {
            ::close(m_Fd);
            m_Fd = -1;
        }
    }
    int m_Fd = -1;
};

/// openDirectoryForUser()'s answer: the folder, and why one it made couldn't be given to its user (empty when nothing
/// failed). The folder is still made and usable then, as the instance lock and the config save have always treated it.
struct OpenedDirectory
{
    DirectoryFd fd;
    std::string handOverError;
};

/// @p dir opened as a folder (O_PATH | O_DIRECTORY, for openat() and fstat()), made first where it's missing (#4152).
/// Walked one component at a time from "/" (or the working directory, for a relative @p dir) with openat(O_NOFOLLOW),
/// so nothing is resolved by path after it was checked, and each step needs only search permission. A link on the way
/// is followed when @p lessPrivileged is false; run as root for a user (#3759), only when root owns both it and the
/// folder holding it, which no one else may write (/home on some systems), and otherwise refuses. A ".." refuses. Each
/// folder this walk made (mkdirat, 0777 less the umask), and that is still that empty folder, is given through its own
/// descriptor to the owner of the deepest folder that already existed, when @p effectiveUid is root's and that folder
/// isn't (ownerForNewFiles()'s rule, #2121); @p failChown stands in for a refused fchown, for tests. @return The
/// folder, or why it couldn't be opened or made.
[[nodiscard]] inline std::expected<OpenedDirectory, std::string>
openDirectoryForUser(const std::filesystem::path& dir, bool lessPrivileged, uid_t effectiveUid, bool failChown = false)
{
    // ".." is refused before anything is normalized: dropping it lexically could check a folder other than the one the
    // kernel would resolve through a link before it (the #4166 review).
    for (const std::filesystem::path& part : dir)
    {
        if (part == "..")
        {
            return std::unexpected(dir.string() + " climbs with \"..\", which TaskSmack doesn't walk");
        }
    }
    // O_PATH: each step needs only search permission on the folder above it, as a by-path open does, not read.
    constexpr int WALK = O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    DirectoryFd at(::open(dir.is_absolute() ? "/" : ".", O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (at.get() < 0)
    {
        return std::unexpected(std::system_category().message(errno));
    }
    std::filesystem::path walked = dir.is_absolute() ? std::filesystem::path("/") : std::filesystem::path();
    std::optional<FileOwner> owner;
    bool ownerKnown = false;
    std::string handOverError;
    for (const std::filesystem::path& part : dir.relative_path())
    {
        if (part.empty() || part == ".")
        {
            continue;
        }
        walked /= part;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
        int next = ::openat(at.get(), part.c_str(), WALK);
        if (next < 0 && errno == ENOENT)
        {
            if (!ownerKnown)
            {
                // The deepest folder that exists, this walk's own descriptor of it: the new folders' owner (#2121).
                struct stat existing = {};
                if (effectiveUid == 0 && ::fstat(at.get(), &existing) == 0 && existing.st_uid != 0)
                {
                    owner = FileOwner{.uid = existing.st_uid, .gid = existing.st_gid};
                }
                ownerKnown = true;
            }
            const bool madeHere = ::mkdirat(at.get(), part.c_str(), S_IRWXU | S_IRWXG | S_IRWXO) == 0;
            if (!madeHere && errno != EEXIST)
            {
                return std::unexpected(walked.string() + " can't be made: " + std::system_category().message(errno));
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
            next = ::openat(at.get(), part.c_str(), WALK);
            // Given to its user only when this walk made it and it's still that folder: this process's, and empty (two
            // links, no subfolders). A folder renamed in its place (EEXIST, or a swap after mkdirat) is left as it is.
            struct stat made = {};
            if (next >= 0 && madeHere && owner.has_value() && ::fstat(next, &made) == 0 && made.st_uid == ::geteuid() &&
                made.st_nlink == 2 && (failChown || ::fchownat(next, "", owner->uid, owner->gid, AT_EMPTY_PATH) != 0) &&
                handOverError.empty())
            {
                handOverError =
                    "couldn't give " + walked.string() + " to its user: " + std::system_category().message(failChown ? EPERM : errno);
            }
        }
        else if (next < 0 && (errno == ELOOP || errno == ENOTDIR))
        {
            // O_NOFOLLOW met a link (ELOOP), or the entry isn't a folder: which, from the entry itself.
            struct stat entry = {};
            if (::fstatat(at.get(), part.c_str(), &entry, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISLNK(entry.st_mode))
            {
                return std::unexpected(walked.string() + " isn't a folder");
            }
            if (lessPrivileged)
            {
                // Run as root for a user, a link is followed only when root owns it and so does the folder holding it,
                // which no one else may write: the user can neither place a link of root's there nor swap one in after
                // this check (the #4166 review). A link in a folder of theirs is refused, root's or not.
                struct stat holder = {};
                if (entry.st_uid != 0 || ::fstat(at.get(), &holder) != 0 || holder.st_uid != 0 ||
                    (holder.st_mode & (S_IWGRP | S_IWOTH)) != 0)
                {
                    return std::unexpected(walked.string() + " is a link of a user's, which TaskSmack run as root doesn't create through");
                }
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
            next = ::openat(at.get(), part.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC); // root's own link, or ours
        }
        if (next < 0)
        {
            return std::unexpected(walked.string() + " can't be opened: " + std::system_category().message(errno));
        }
        at = DirectoryFd(next);
    }
    return OpenedDirectory{.fd = std::move(at), .handOverError = std::move(handOverError)};
}

/// Creates @p dir and any missing ancestors the way UserConfig's save does (#4089), through openDirectoryForUser()
/// (#4152): when @p lessPrivileged (run as root for a user, #3759) nothing is created through a link of theirs on the
/// way, and each directory made while root is given to the owner of its nearest existing ancestor, so the user's next
/// normal run can use it. @p effectiveUid is the process's (::geteuid(), or a test's). @return Empty when @p dir exists
/// afterwards and what was made is the user's, else why not.
[[nodiscard]] inline std::string createDirectoriesForUser(const std::filesystem::path& dir, bool lessPrivileged, uid_t effectiveUid)
{
    auto opened = openDirectoryForUser(dir, lessPrivileged, effectiveUid);
    if (!opened.has_value())
    {
        return std::move(opened.error());
    }
    return std::move(opened->handOverError);
}

} // namespace App::NewFileOwner

#endif
