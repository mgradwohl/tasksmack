#pragma once

/// @file NewFileOwner.h
/// @brief Who should own the files and directories TaskSmack creates under the user's config directory
/// (#2121, #2437). Running as root with another user's HOME (sudo -E, as the elevation notice suggests),
/// anything created there would be root's, so the user's next normal run could neither read nor replace
/// it. UserConfig::save() and InstanceLock hand what they create to that user. POSIX only.

#ifndef _WIN32

#include <filesystem>
#include <optional>

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

} // namespace App::NewFileOwner

#endif
