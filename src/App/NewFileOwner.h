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

/// @p dir opened as a folder (O_RDONLY | O_DIRECTORY), made first where it's missing (#4152). Walked one component at a
/// time from "/" (or the working directory, for a relative @p dir) with openat(O_NOFOLLOW), so nothing is resolved by
/// path after it was checked: a link swapped in between steps isn't followed. A link on the way is followed only when
/// root owns it (/home on some systems) or @p lessPrivileged is false; one of a user's, when run as root for a user
/// (#3759), refuses. Each folder made (mkdirat, 0777 less the umask) is given, through its own descriptor, to the
/// owner of the deepest folder that already existed, when @p effectiveUid is root's and that folder isn't
/// (ownerForNewFiles()'s rule, #2121); @p failChown stands in for a refused fchown, for tests. @return The folder,
/// or why it couldn't be opened or made.
[[nodiscard]] inline std::expected<OpenedDirectory, std::string>
openDirectoryForUser(const std::filesystem::path& dir, bool lessPrivileged, uid_t effectiveUid, bool failChown = false)
{
    const std::filesystem::path normal = dir.lexically_normal();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    DirectoryFd at(::open(normal.is_absolute() ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (at.get() < 0)
    {
        return std::unexpected(std::system_category().message(errno));
    }
    std::filesystem::path walked = normal.is_absolute() ? std::filesystem::path("/") : std::filesystem::path();
    std::optional<FileOwner> owner;
    bool ownerKnown = false;
    std::string handOverError;
    for (const std::filesystem::path& part : normal.relative_path())
    {
        if (part.empty() || part == ".")
        {
            continue;
        }
        if (part == "..")
        {
            return std::unexpected(normal.string() + " climbs with \"..\", which TaskSmack doesn't walk");
        }
        walked /= part;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
        int next = ::openat(at.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
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
            if (::mkdirat(at.get(), part.c_str(), S_IRWXU | S_IRWXG | S_IRWXO) != 0 && errno != EEXIST)
            {
                return std::unexpected(walked.string() + " can't be made: " + std::system_category().message(errno));
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
            next = ::openat(at.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next >= 0 && owner.has_value() && (failChown || ::fchown(next, owner->uid, owner->gid) != 0) && handOverError.empty())
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
            if (lessPrivileged && entry.st_uid != 0)
            {
                return std::unexpected(walked.string() + " is a link of a user's, which TaskSmack run as root doesn't create through");
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
            next = ::openat(at.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); // root's own link, or ours
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
