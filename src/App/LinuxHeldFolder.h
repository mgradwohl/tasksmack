#pragma once

// The config folder of a TaskSmack run as root for a user (sudo, pkexec, doas), held open from startup and the one way
// into it (#4167, #4257), as an elevated Windows session holds its handed-over folder (Platform::Windows::PinnedFolder,
// #3774): config.toml, the instance lock and the user themes are each opened relative to this folder's descriptor,
// never by a path from "/" down, so a link the user swaps in for the folder, or for anything in it, after the startup
// check can't send root's reads or writes anywhere else.
//
// The folder is walked and made for the user by NewFileOwner::openDirectoryForUser() (#4152). Inside it, nothing is
// followed or opened that isn't the user's own plain file: each name is opened with O_NOFOLLOW (and O_NONBLOCK, so a
// FIFO can't block a read), and only a regular file with one link, owned by the folder's owner, and no larger than
// MAX_FILE_BYTES is read. What root makes here (a new file, the themes folder, the lock) is given to that owner (#2121).
//
// Linux only: main.cpp holds one when SessionUser::invokingUserHome() names a user, and Core::ConfigDirOverride keeps a
// weak reference to it (adoptHandedOver()).

#if defined(__linux__)

#include "App/NewFileOwner.h"
#include "Platform/IHandedOverFolder.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace App
{

class LinuxHeldFolder final : public Platform::IHandedOverFolder
{
  public:
    /// The largest file read through the folder: config.toml's limit (UserConfig, #4546), and far above any theme.
    static constexpr std::size_t MAX_FILE_BYTES = std::size_t{4} * 1024U * 1024U;

    /// @p dir walked and made for its user by NewFileOwner::openDirectoryForUser() (as root for a user, through no link
    /// of theirs), and held. An error when it can't be opened or made, or a folder made for the user couldn't be given
    /// to them: then nothing is held, and the session fails closed (defaults, nothing saved), as Windows' does.
    [[nodiscard]] static std::expected<std::shared_ptr<LinuxHeldFolder>, std::string> hold(const std::filesystem::path& dir,
                                                                                           uid_t effectiveUid);

    /// Holds @p folder (an O_PATH descriptor on @p path) for a process whose effective uid is @p effectiveUid. Whose the
    /// folder is comes from the descriptor (fstat), never from the path. Tests make one directly.
    LinuxHeldFolder(NewFileOwner::DirectoryFd folder, std::filesystem::path path, uid_t effectiveUid);

    [[nodiscard]] const std::filesystem::path& path() const noexcept override;
    /// The file's descriptor, as a void* (the interface's handle): (void*)(intptr_t)fd, which the caller closes.
    [[nodiscard]] std::expected<void*, std::string> openLockFile(const std::filesystem::path& name) const override;
    [[nodiscard]] std::expected<std::optional<std::string>, std::string> readFile(const std::filesystem::path& relative) const override;
    [[nodiscard]] std::expected<void, std::string> replaceFile(const std::filesystem::path& name, std::string_view contents) const override;
    [[nodiscard]] std::expected<void, std::string> createFolder(const std::filesystem::path& name) const override;
    [[nodiscard]] std::expected<std::vector<std::filesystem::path>, std::string>
    listFiles(const std::filesystem::path& name) const override;

    /// The folder's descriptor (O_PATH), for UserConfig's save (writeIntoFolderFd(), #4546), which keeps the old file's
    /// owner, mode and access control list. Valid while this object lives.
    [[nodiscard]] int posixFolderFd() const noexcept override
    {
        return m_Folder.get();
    }

    /// Whose the folder is, from its descriptor: what root makes in it is given to them, and only their files are read.
    [[nodiscard]] const NewFileOwner::FileOwner& owner() const noexcept
    {
        return m_Owner;
    }

  private:
    /// The folder holding @p relative's last component, walked from the held folder one component at a time with
    /// O_NOFOLLOW (a link on the way is refused), and that last component's name. nullopt when a folder on the way isn't
    /// there. An error for a name that isn't a plain relative path ("", "/x", "..", ".").
    struct Parent
    {
        NewFileOwner::DirectoryFd folder; ///< Empty (-1) for the held folder itself
        std::string leaf;
    };
    [[nodiscard]] std::expected<std::optional<Parent>, std::string> walkTo(const std::filesystem::path& relative) const;
    /// The descriptor *at() calls take for @p parent: its own, or the held folder's.
    [[nodiscard]] int at(const Parent& parent) const noexcept
    {
        return parent.folder.get() >= 0 ? parent.folder.get() : m_Folder.get();
    }
    /// Whether root is acting for someone else here: what it makes must then be given to the folder's owner.
    [[nodiscard]] bool givesToOwner() const noexcept
    {
        return m_EffectiveUid == 0 && m_Owner.uid != 0;
    }

    NewFileOwner::DirectoryFd m_Folder;
    std::filesystem::path m_Path;
    uid_t m_EffectiveUid = 0;
    NewFileOwner::FileOwner m_Owner{.uid = 0, .gid = 0};
};

} // namespace App

#endif // __linux__
