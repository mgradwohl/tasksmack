#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <sys/stat.h>
#include <sys/types.h>

/// A Linux root session's own default dump folder (#4181): <root's home>/TaskSmack-dumps, 0700 and root's, made by
/// descriptor so nothing the user's environment or a link they planted can steer where it is or what it is.
namespace Platform::RootDumpFolder
{

/// The folder's name in root's home.
inline constexpr std::string_view FOLDER_NAME = "TaskSmack-dumps";

/// Makes @p name in @p parent, or opens the one there, as a folder only @p owner may open (0700, theirs): @p parent must
/// be @p owner's own folder no group or other may write, and @p name a folder, never a link (O_NOFOLLOW). A looser
/// mode on one that's already @p owner's is tightened to 0700. @return Nullopt when it's ready, else why not.
[[nodiscard]] std::optional<std::string> makePrivateFolder(const std::filesystem::path& parent, std::string_view name, uid_t owner);

/// Run as root: why gcore may not write a dump into @p directory, or nullopt when it may (#4176): every component from
/// "/" down a plain folder only root may change (LinuxCoreDump::rootOnlyFolderRefusal()), on a filesystem that says
/// what it is and isn't a user's (LinuxCoreDump::userMountRefusal(); fail closed). createDump()'s check, and
/// rootDumpFolder()'s, so the default offered is one Create will take (the #4190 review).
[[nodiscard]] std::optional<std::string> rootDumpRefusal(const std::filesystem::path& directory);

/// rootDumpRefusal() with the lstat() and the untrusted-filesystem check (RootTools::RootFs::onUntrustedFilesystem) a test
/// gives (#4648): NFS, CIFS/SMB, CEPH, AFS, 9p, FUSE or a nosuid mount, on the folder or any folder above it, is refused
/// as it is for root's tools (#4554, #4572): its owners and modes come from elsewhere, so the root-only check proves
/// nothing there, and the server side, or a local user with an NFSv4 ACL, can swap the dump's name for a link.
[[nodiscard]] std::optional<std::string> rootDumpRefusal(const std::filesystem::path& directory,
                                                         const std::function<int(const char*, struct stat*)>& lstatOf,
                                                         const std::function<bool(const std::string&)>& onUntrustedFilesystem);

/// @p homeText without a trailing "/", then FOLDER_NAME made there for @p owner (makePrivateFolder()), if @p refusal takes
/// it (rootDumpRefusal() for root's). Empty, with a warning once a session, when either refuses (#4262's seam: a test
/// gives a scratch home and its own refusal; rootDumpFolder() gives root's home, 0 and rootDumpRefusal()).
[[nodiscard]] std::filesystem::path
rootDumpFolderIn(std::string homeText, uid_t owner, const std::function<std::optional<std::string>(const std::filesystem::path&)>& refusal);

/// Root's default dump folder, made when it isn't there yet (makePrivateFolder() in root's passwd home, never HOME,
/// which sudo -E keeps the user's), and only if rootDumpRefusal() takes it. Empty when it can't be made or isn't taken
/// (a root home that is a link, as on ostree systems, or on a nodev mount): the confirm then waits for Save dump to.... Touches the
/// file system: on the dump confirm's worker, never the UI thread.
[[nodiscard]] std::filesystem::path rootDumpFolder();

} // namespace Platform::RootDumpFolder
