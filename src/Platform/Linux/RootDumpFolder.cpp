#include "RootDumpFolder.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
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
    const std::filesystem::path home(entry.pw_dir);
    if (const std::optional<std::string> refusal = makePrivateFolder(home, FOLDER_NAME, 0); refusal.has_value())
    {
        spdlog::warn("No default dump folder: {} (#4181)", *refusal);
        return {};
    }
    return home / FOLDER_NAME;
}

} // namespace Platform::RootDumpFolder
