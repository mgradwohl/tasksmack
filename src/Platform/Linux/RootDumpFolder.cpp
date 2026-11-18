#include "RootDumpFolder.h"

#include "LinuxCoreDump.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cerrno>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

namespace Platform::RootDumpFolder
{

namespace
{

/// Closes a descriptor when it goes out of scope.
class Descriptor
{
  public:
    explicit Descriptor(int fd) noexcept : m_Fd(fd)
    {}
    ~Descriptor()
    {
        if (m_Fd >= 0)
        {
            ::close(m_Fd);
        }
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&&) = delete;
    Descriptor& operator=(Descriptor&&) = delete;
    [[nodiscard]] int get() const noexcept
    {
        return m_Fd;
    }

  private:
    int m_Fd;
};

} // namespace

std::optional<std::string> makePrivateFolder(const std::filesystem::path& parent, std::string_view name, uid_t owner)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const Descriptor parentFd(::open(parent.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (parentFd.get() < 0)
    {
        return std::format("couldn't open {}: {}", parent.string(), std::generic_category().message(errno));
    }
    struct stat parentInfo = {};
    if (::fstat(parentFd.get(), &parentInfo) != 0 || parentInfo.st_uid != owner || (parentInfo.st_mode & (S_IWGRP | S_IWOTH)) != 0)
    {
        return std::format("{} isn't a folder only its owner may change", parent.string());
    }
    const std::string folderName(name);
    if (::mkdirat(parentFd.get(), folderName.c_str(), S_IRWXU) != 0 && errno != EEXIST)
    {
        return std::format("couldn't make {}: {}", (parent / folderName).string(), std::generic_category().message(errno));
    }
    // A link, or anything but a folder, at that name is refused, never followed (ELOOP, ENOTDIR).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const Descriptor folder(::openat(parentFd.get(), folderName.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (folder.get() < 0)
    {
        return std::format("couldn't open {} as a folder: {}", (parent / folderName).string(), std::generic_category().message(errno));
    }
    struct stat info = {};
    if (::fstat(folder.get(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != owner)
    {
        return std::format("{} isn't its owner's own folder", (parent / folderName).string());
    }
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0 && ::fchmod(folder.get(), S_IRWXU) != 0)
    {
        return std::format("couldn't make {} owner-only: {}", (parent / folderName).string(), std::generic_category().message(errno));
    }
    return std::nullopt;
}

std::optional<std::string> rootDumpRefusal(const std::filesystem::path& directory)
{
    std::optional<std::string> refusal = LinuxCoreDump::rootOnlyFolderRefusal(directory, &::lstat);
    if (refusal.has_value())
    {
        return refusal;
    }
    // Fail closed: a filesystem that won't say what it is (a FUSE daemon can refuse statfs) may be a user's (the second
    // #4177 review).
    struct statvfs vfs = {};
    struct statfs fs = {};
    if (::statvfs(directory.c_str(), &vfs) != 0 || ::statfs(directory.c_str(), &fs) != 0)
    {
        return std::format("TaskSmack can't tell what filesystem {} is on", directory.string());
    }
    return LinuxCoreDump::userMountRefusal(directory, vfs.f_flag, static_cast<long>(fs.f_type));
}

std::filesystem::path
rootDumpFolderIn(std::string homeText, uid_t owner, const std::function<std::optional<std::string>(const std::filesystem::path&)>& refusal)
{
    // Without a trailing "/": one would make open() follow a link the O_NOFOLLOW check is there to refuse.
    while (homeText.size() > 1 && homeText.ends_with('/'))
    {
        homeText.pop_back();
    }
    const std::filesystem::path home(homeText);
    std::filesystem::path folder = home / FOLDER_NAME;
    std::optional<std::string> refused = makePrivateFolder(home, FOLDER_NAME, owner);
    if (!refused.has_value())
    {
        refused = refusal(folder);
    }
    if (refused.has_value())
    {
        static std::atomic_flag warned; // once a session, not at every confirm
        if (!warned.test_and_set())
        {
            spdlog::warn("No default dump folder: {} (#4181)", *refused);
        }
        return {};
    }
    return folder;
}

std::filesystem::path rootDumpFolder()
{
    passwd entry{};
    passwd* found = nullptr;
    std::vector<char> buffer(16384);
    if (::getpwuid_r(0, &entry, buffer.data(), buffer.size(), &found) != 0 || found == nullptr || entry.pw_dir == nullptr ||
        entry.pw_dir[0] != '/')
    {
        spdlog::warn("No default dump folder: root's home isn't in the passwd database (#4181)");
        return {};
    }
    return rootDumpFolderIn(entry.pw_dir, 0, &rootDumpRefusal);
}

} // namespace Platform::RootDumpFolder
