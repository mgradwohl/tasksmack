// Keep this translation unit parseable on non-Linux platforms (e.g. Windows clangd) by compiling the implementation
// only when targeting Linux.
#if defined(__linux__)

#include "LinuxHeldFolder.h"

#include "App/NewFileOwner.h"
#include "Platform/IHandedOverFolder.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <dirent.h>
#include <stdio.h> // NOLINT(modernize-deprecated-headers) - renameat is POSIX, declared only in stdio.h
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// NOLINTBEGIN(misc-include-cleaner) - POSIX: include-cleaner lacks mappings for the open flags and AT_* constants
#include <fcntl.h>
// NOLINTEND(misc-include-cleaner)

namespace App
{

namespace
{

[[nodiscard]] std::string errnoText(int error)
{
    return std::system_category().message(error);
}

/// Whether @p name is one plain path component: not empty, ".", ".." or anything with a separator in it.
[[nodiscard]] bool isPlainComponent(std::string_view name) noexcept
{
    return !name.empty() && name != "." && name != ".." && !name.contains('/');
}

/// Writes all of @p text to @p fd, retrying a short or interrupted write; false (errno set) on failure.
[[nodiscard]] bool writeAll(int fd, std::string_view text) noexcept
{
    while (!text.empty())
    {
        const ssize_t written = ::write(fd, text.data(), text.size()); // NOLINT(misc-include-cleaner) - ssize_t from <sys/types.h>
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        text.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

} // namespace

std::expected<std::shared_ptr<LinuxHeldFolder>, std::string> LinuxHeldFolder::hold(const std::filesystem::path& dir, uid_t effectiveUid)
{
    auto opened = NewFileOwner::openDirectoryForUser(dir, /*lessPrivileged=*/true, effectiveUid);
    if (!opened.has_value())
    {
        return std::unexpected(std::move(opened.error()));
    }
    // A folder made for the user that couldn't be given to them is root's: the save by descriptor always refused to
    // write there (#4152), and a held folder isn't one either.
    if (!opened->handOverError.empty())
    {
        return std::unexpected(std::move(opened->handOverError));
    }
    return std::make_shared<LinuxHeldFolder>(std::move(opened->fd), Platform::handedOverFolderPath(dir), effectiveUid);
}

LinuxHeldFolder::LinuxHeldFolder(NewFileOwner::DirectoryFd folder, std::filesystem::path path, uid_t effectiveUid)
    : m_Folder(std::move(folder)), m_Path(std::move(path)), m_EffectiveUid(effectiveUid), m_Owner{.uid = effectiveUid, .gid = ::getegid()}
{
    if (struct stat info = {}; m_Folder.get() >= 0 && ::fstat(m_Folder.get(), &info) == 0)
    {
        m_Owner = NewFileOwner::FileOwner{.uid = info.st_uid, .gid = info.st_gid};
    }
}

const std::filesystem::path& LinuxHeldFolder::path() const noexcept
{
    return m_Path;
}

std::expected<std::optional<LinuxHeldFolder::Parent>, std::string> LinuxHeldFolder::walkTo(const std::filesystem::path& relative) const
{
    if (relative.empty() || relative.is_absolute())
    {
        return std::unexpected(std::format("\"{}\" isn't a name inside the held folder", relative.string()));
    }
    std::vector<std::string> parts;
    for (const std::filesystem::path& part : relative)
    {
        const std::string text = part.string();
        if (!isPlainComponent(text))
        {
            return std::unexpected(std::format("\"{}\" isn't a plain name inside the held folder", relative.string()));
        }
        parts.push_back(text);
    }
    Parent parent{.folder = NewFileOwner::DirectoryFd(), .leaf = parts.back()};
    parts.pop_back();
    for (const std::string& part : parts)
    {
        // A link on the way is refused, never followed: O_NOFOLLOW on an O_PATH open gives the link itself, which the
        // S_ISDIR check below then refuses.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
        NewFileOwner::DirectoryFd next(::openat(at(parent), part.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (next.get() < 0)
        {
            if (errno == ENOENT)
            {
                return std::nullopt;
            }
            return std::unexpected(
                std::format("{} on the way to {} isn't a folder of its own ({})", part, relative.string(), errnoText(errno)));
        }
        if (struct stat info = {}; ::fstat(next.get(), &info) != 0 || !S_ISDIR(info.st_mode))
        {
            return std::unexpected(std::format("{} on the way to {} isn't a folder", part, relative.string()));
        }
        parent.folder = std::move(next);
    }
    return parent;
}

std::expected<std::optional<std::string>, std::string> LinuxHeldFolder::readFile(const std::filesystem::path& relative) const
{
    auto walked = walkTo(relative);
    if (!walked.has_value())
    {
        return std::unexpected(std::move(walked.error()));
    }
    if (!walked->has_value())
    {
        return std::nullopt;
    }
    const Parent& parent = **walked;
    // O_NONBLOCK: a FIFO planted here opens at once and is refused below, instead of blocking startup for ever (#4257).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const NewFileOwner::DirectoryFd file(
        ::openat(at(parent), parent.leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC));
    if (file.get() < 0)
    {
        if (errno == ENOENT)
        {
            return std::nullopt;
        }
        if (errno == ELOOP)
        {
            return std::unexpected(relative.string() + " is a link, which isn't followed in a held folder");
        }
        return std::unexpected(relative.string() + " can't be opened: " + errnoText(errno));
    }
    struct stat info = {};
    if (::fstat(file.get(), &info) != 0)
    {
        return std::unexpected(relative.string() + " can't be checked: " + errnoText(errno));
    }
    // Only the owner's own plain file: a hard link to another file (/root's, a device's), a FIFO or a folder is refused,
    // and so is a file of someone else's, so nothing root can read and they can't is read as theirs (#4257).
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1)
    {
        return std::unexpected(relative.string() + " isn't a plain file of its own (a hard link, or not a file)");
    }
    if (info.st_uid != m_Owner.uid)
    {
        return std::unexpected(relative.string() + " isn't owned by its folder's owner");
    }
    if (info.st_size < 0 || std::cmp_greater(info.st_size, MAX_FILE_BYTES))
    {
        return std::unexpected(relative.string() + " is too large to be read");
    }
    std::string text;
    text.reserve(static_cast<std::size_t>(info.st_size));
    std::array<char, 16384> buffer{};
    for (;;)
    {
        const ssize_t got = ::read(file.get(), buffer.data(), buffer.size()); // NOLINT(misc-include-cleaner) - ssize_t from <sys/types.h>
        if (got < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return std::unexpected(relative.string() + " can't be read: " + errnoText(errno));
        }
        if (got == 0)
        {
            break;
        }
        text.append(buffer.data(), static_cast<std::size_t>(got));
        if (text.size() > MAX_FILE_BYTES)
        {
            return std::unexpected(relative.string() + " is too large to be read"); // it grew while being read
        }
    }
    return text;
}

std::expected<void, std::string> LinuxHeldFolder::replaceFile(const std::filesystem::path& name, std::string_view contents) const
{
    auto walked = walkTo(name);
    if (!walked.has_value())
    {
        return std::unexpected(std::move(walked.error()));
    }
    if (!walked->has_value())
    {
        return std::unexpected("a folder on the way to " + name.string() + " isn't there");
    }
    const Parent& parent = **walked;
    const int folder = at(parent);

    // The file it replaces, if any, must be the owner's own plain file, as readFile() requires; its permissions are kept.
    mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    if (struct stat existing = {}; ::fstatat(folder, parent.leaf.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0)
    {
        if (!S_ISREG(existing.st_mode) || existing.st_nlink != 1 || existing.st_uid != m_Owner.uid)
        {
            return std::unexpected(name.string() + " isn't a plain file of the folder owner's own; not replacing it");
        }
        mode = static_cast<mode_t>(existing.st_mode & 07777U);
    }
    else if (errno != ENOENT)
    {
        return std::unexpected(name.string() + " can't be checked: " + errnoText(errno));
    }

    // The staging file: made here exclusively, never through a link, owner-only until it is complete, and the owner's
    // from the start, so one they move aside while it is being written was never root's.
    std::random_device random;
    std::string staging;
    int fd = -1;
    for (int attempt = 0; attempt < 8 && fd < 0; ++attempt)
    {
        staging = std::format(".tasksmack-{:08x}.tmp", random());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
        fd = ::openat(folder, staging.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (fd < 0 && errno != EEXIST)
        {
            break;
        }
    }
    if (fd < 0)
    {
        return std::unexpected("couldn't create a staging file beside " + name.string() + ": " + errnoText(errno));
    }
    const NewFileOwner::DirectoryFd staged(fd);
    const auto fail = [folder, &staging](std::string why) -> std::expected<void, std::string>
    {
        ::unlinkat(folder, staging.c_str(), 0);
        return std::unexpected(std::move(why));
    };
    if (givesToOwner() && ::fchown(staged.get(), m_Owner.uid, m_Owner.gid) != 0)
    {
        return fail("couldn't give the new " + name.string() + " to its user: " + errnoText(errno));
    }
    if (!writeAll(staged.get(), contents) || ::fchmod(staged.get(), mode) != 0 || ::fsync(staged.get()) != 0)
    {
        return fail("couldn't write the new " + name.string() + ": " + errnoText(errno));
    }
    if (::renameat(folder, staging.c_str(), folder, parent.leaf.c_str()) != 0)
    {
        return fail("couldn't replace " + name.string() + ": " + errnoText(errno));
    }
    // The rename on disk too: an O_PATH descriptor can't be fsync'd, so the folder is opened for reading.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    if (const NewFileOwner::DirectoryFd readable(::openat(folder, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)); readable.get() >= 0)
    {
        ::fsync(readable.get());
    }
    return {};
}

std::expected<void, std::string> LinuxHeldFolder::createFolder(const std::filesystem::path& name) const
{
    auto walked = walkTo(name);
    if (!walked.has_value())
    {
        return std::unexpected(std::move(walked.error()));
    }
    if (!walked->has_value())
    {
        return std::unexpected("a folder on the way to " + name.string() + " isn't there");
    }
    const Parent& parent = **walked;
    const int folder = at(parent);
    if (::mkdirat(folder, parent.leaf.c_str(), S_IRWXU | S_IRWXG | S_IRWXO) != 0)
    {
        if (errno != EEXIST)
        {
            return std::unexpected("couldn't create " + name.string() + ": " + errnoText(errno));
        }
        if (struct stat info = {}; ::fstatat(folder, parent.leaf.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(info.st_mode))
        {
            return std::unexpected(name.string() + " is there, but isn't a folder");
        }
        return {};
    }
    if (!givesToOwner())
    {
        return {};
    }
    // Given through its own descriptor (AT_EMPTY_PATH), and only while it is still the folder this call made (root's):
    // one the user swapped in meanwhile is left alone.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const NewFileOwner::DirectoryFd made(::openat(folder, parent.leaf.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat info = {};
    if (made.get() < 0 || ::fstat(made.get(), &info) != 0 || info.st_uid != 0)
    {
        return std::unexpected(name.string() + " changed while it was being made; not giving it to its user");
    }
    if (::fchownat(made.get(), "", m_Owner.uid, m_Owner.gid, AT_EMPTY_PATH) != 0)
    {
        return std::unexpected("couldn't give " + name.string() + " to its user: " + errnoText(errno));
    }
    return {};
}

std::expected<std::vector<std::filesystem::path>, std::string> LinuxHeldFolder::listFiles(const std::filesystem::path& name) const
{
    int listed = -1;
    if (name.empty())
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
        listed = ::openat(m_Folder.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }
    else
    {
        auto walked = walkTo(name);
        if (!walked.has_value())
        {
            return std::unexpected(std::move(walked.error()));
        }
        if (!walked->has_value())
        {
            return std::vector<std::filesystem::path>{};
        }
        const Parent& parent = **walked;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
        listed = ::openat(at(parent), parent.leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (listed < 0 && errno == ENOENT)
        {
            return std::vector<std::filesystem::path>{};
        }
    }
    if (listed < 0)
    {
        return std::unexpected((name.empty() ? m_Path.string() : name.string()) + " can't be listed: " + errnoText(errno));
    }
    // fdopendir() owns the descriptor from here; closedir() closes both.
    DIR* const dir = ::fdopendir(listed);
    if (dir == nullptr)
    {
        const int error = errno;
        ::close(listed);
        return std::unexpected(name.string() + " can't be listed: " + errnoText(error));
    }
    const int listedFd = ::dirfd(dir);
    if (listedFd < 0)
    {
        ::closedir(dir);
        return std::unexpected(name.string() + " can't be listed");
    }
    std::vector<std::filesystem::path> names;
    for (const dirent* entry = ::readdir(dir); entry != nullptr; entry = ::readdir(dir)) // NOLINT(concurrency-mt-unsafe) - a DIR of its own
    {
        const std::string_view entryName(static_cast<const char*>(entry->d_name));
        if (entryName == "." || entryName == "..")
        {
            continue;
        }
        // Plain files only: no folders, and no links, which lstat-style checking sees as links.
        if (struct stat info = {}; ::fstatat(listedFd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(info.st_mode))
        {
            names.emplace_back(std::string(entryName));
        }
    }
    ::closedir(dir);
    std::ranges::sort(names);
    return names;
}

std::expected<void*, std::string> LinuxHeldFolder::openLockFile(const std::filesystem::path& name) const
{
    auto walked = walkTo(name);
    if (!walked.has_value())
    {
        return std::unexpected(std::move(walked.error()));
    }
    if (!walked->has_value())
    {
        return std::unexpected("a folder on the way to " + name.string() + " isn't there");
    }
    const Parent& parent = **walked;
    // O_CLOEXEC: a program TaskSmack starts mustn't inherit the lock. O_NOFOLLOW: never a link's target (#4156).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const int fd = ::openat(at(parent), parent.leaf.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (fd < 0)
    {
        return std::unexpected(name.string() + " can't be opened: " + errnoText(errno));
    }
    // Only a plain file with no other name: a hard link to another file planted as the lock would otherwise be locked,
    // and handed to the user below (the #4166 review).
    struct stat info = {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
    {
        ::close(fd);
        return std::unexpected(name.string() + " isn't a plain file of its own (another name links to it); not using it as the lock");
    }
    // Root hands the user only a lock file of root's: one this run made, or one an older root run left (#2437). Another
    // account's file renamed in here is neither locked nor handed over (#4805): giving it to the user would let them
    // read and change someone else's file through a sudo run.
    if (givesToOwner() && info.st_uid != m_Owner.uid && info.st_uid != 0)
    {
        ::close(fd);
        return std::unexpected(name.string() + " belongs to another account (uid " + std::to_string(info.st_uid) +
                               "); not using it as the lock");
    }
    // The user's, whether this run made it or an older one left it root's: their next run opens it read-write (#2437).
    if (givesToOwner() && info.st_uid != m_Owner.uid && ::fchown(fd, m_Owner.uid, m_Owner.gid) != 0)
    {
        spdlog::warn("Couldn't give {} to its user: {}", (m_Path / name).string(), errnoText(errno));
    }
    // The interface's handle is a void*: the descriptor, which the caller closes.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr) - IHandedOverFolder's handle type
    return reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
}

} // namespace App

#endif // __linux__
