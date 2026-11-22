#pragma once

// Whether the user a root session runs for could open a folder (#4765): a root session's dump goes into a root-only
// folder (#4176, #4181), and its result's "Open folder" hands it to that user's file manager (#2461), which couldn't open
// it. By the permission bits along the path; no ACL is read, so a folder an ACL opens to the user reads as closed, and the
// result names it instead of opening it, which is no worse.

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>

namespace Platform::UserFolderAccess
{

/// A path's owner, group and mode, as stat() gives them.
struct PathModes
{
    uid_t uid = 0;
    gid_t gid = 0;
    mode_t mode = 0;
};

/// stat() of a path (following links, as a file manager does), nullopt when it can't be read.
using StatPath = std::function<std::optional<PathModes>(const std::string&)>;

/// Read and search, as the bits a folder's mode gives for its owner, group or others.
inline constexpr unsigned READ = 4;
inline constexpr unsigned SEARCH = 1;

/// Whether the account @p uid, of primary group @p gid and the supplementary @p groups, has every bit of @p need
/// (READ, SEARCH) on a node with @p modes: the owner's bits when it owns it, else the group's when it is in its group,
/// else the others'. Root (uid 0) has every one.
[[nodiscard]] constexpr bool allows(const PathModes& modes, uid_t uid, gid_t gid, std::span<const gid_t> groups, unsigned need) noexcept
{
    if (uid == 0)
    {
        return true;
    }
    unsigned bits = modes.mode & 7U;
    if (modes.uid == uid)
    {
        bits = (modes.mode >> 6U) & 7U;
    }
    else if (modes.gid == gid || std::ranges::contains(groups, modes.gid))
    {
        bits = (modes.mode >> 3U) & 7U;
    }
    return (bits & need) == need;
}

/// Whether that account could open the folder @p folder (absolute): search every folder above it and read and search
/// it. False when any of them can't be read, or @p folder isn't absolute.
[[nodiscard]] inline bool
canOpenFolder(const std::filesystem::path& folder, uid_t uid, gid_t gid, std::span<const gid_t> groups, const StatPath& stat)
{
    if (!folder.is_absolute())
    {
        return false;
    }
    // "/", then each folder in turn; the last is the folder itself, which must be read too. A trailing "/" is dropped:
    // it names the same folder.
    std::filesystem::path normal = folder.lexically_normal();
    if (normal.has_relative_path() && !normal.has_filename())
    {
        normal = normal.parent_path();
    }
    std::filesystem::path prefix;
    for (auto part = normal.begin(); part != normal.end(); ++part)
    {
        prefix /= *part;
        const std::optional<PathModes> modes = stat(prefix.string());
        if (!modes.has_value())
        {
            return false;
        }
        const unsigned need = std::next(part) == normal.end() ? READ | SEARCH : SEARCH;
        if (!allows(*modes, uid, gid, groups, need))
        {
            return false;
        }
    }
    return true;
}

/// stat() itself.
[[nodiscard]] inline std::optional<PathModes> statPath(const std::string& path)
{
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0)
    {
        return std::nullopt;
    }
    return PathModes{.uid = info.st_uid, .gid = info.st_gid, .mode = info.st_mode};
}

} // namespace Platform::UserFolderAccess
