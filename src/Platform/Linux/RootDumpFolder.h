#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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

/// Root's default dump folder, made when it isn't there yet (makePrivateFolder() in root's passwd home, never HOME,
/// which sudo -E keeps the user's). Empty when it can't be made: the confirm then waits for Save dump to.... Touches the
/// file system: on the dump confirm's worker, never the UI thread.
[[nodiscard]] std::filesystem::path rootDumpFolder();

} // namespace Platform::RootDumpFolder
