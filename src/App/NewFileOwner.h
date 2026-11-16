#pragma once

/// @file NewFileOwner.h
/// @brief Who should own the files and directories TaskSmack creates under the user's config directory
/// (#2121, #2437). Running as root with another user's HOME (sudo -E, as the elevation notice suggests),
/// anything created there would be root's, so the user's next normal run could neither read nor replace
/// it. UserConfig::save() and InstanceLock hand what they create to that user. POSIX only.

#ifndef _WIN32

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
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

/// Creates @p dir and any missing ancestors the way UserConfig's save does (#4089): when @p lessPrivileged (run as root
/// for a user, #3759) nothing is created through a link of theirs on the way, and each directory made while root is
/// given to the owner of its nearest existing ancestor (ownerForNewFiles(), #2121), so the user's next normal run can
/// use it. @p effectiveUid is the process's (::geteuid(), or a test's). @return Empty when @p dir exists afterwards,
/// else why not.
[[nodiscard]] inline std::string createDirectoriesForUser(const std::filesystem::path& dir, bool lessPrivileged, uid_t effectiveUid)
{
    if (lessPrivileged)
    {
        if (const std::optional<std::filesystem::path> link = userOwnedLinkOnPath(dir); link.has_value())
        {
            return link->string() + " is a link of a user's, which TaskSmack run as root doesn't create through";
        }
    }
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec))
    {
        return {};
    }
    const std::optional<FileOwner> owner = ownerForNewFiles(dir, effectiveUid);
    // The directories about to be made, outermost first, to hand to the user afterwards.
    std::vector<std::filesystem::path> created;
    for (std::filesystem::path at = dir; !at.empty() && at != at.parent_path() && !std::filesystem::exists(at, ec); at = at.parent_path())
    {
        created.insert(created.begin(), at);
    }
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        return ec.message();
    }
    if (owner.has_value())
    {
        for (const std::filesystem::path& made : created)
        {
            if (!chownCreatedDirectory(made, *owner))
            {
                return "can't give " + made.string() + " to its user";
            }
        }
    }
    return {};
}

} // namespace App::NewFileOwner

#endif
