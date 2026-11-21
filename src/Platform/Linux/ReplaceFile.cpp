#include "ReplaceFile.h"

#include "PosixGuards.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h> // NOLINT(misc-include-cleaner) - ssize_t, which clang-tidy 23 maps to no header
#include <unistd.h>

namespace Platform::ReplaceFile
{

namespace
{

/// How many unpredictable names are tried before giving up: each collision is someone else's file.
constexpr int NAME_ATTEMPTS = 8;

[[nodiscard]] std::string errnoText(int error)
{
    return std::system_category().message(error);
}

/// A staging name beside @p path no one can guess in advance: ".<name>.tasksmack-<64 random bits>.tmp".
[[nodiscard]] std::filesystem::path stagingName(const std::filesystem::path& path, std::random_device& random)
{
    const std::uint64_t bits = (static_cast<std::uint64_t>(random()) << 32U) ^ static_cast<std::uint64_t>(random());
    return path.parent_path() / std::format(".{}.tasksmack-{:016x}.tmp", path.filename().string(), bits);
}

/// Writes all of @p text to @p fd, across short writes and EINTR. errno is kept on failure.
[[nodiscard]] bool writeAll(int fd, std::string_view text) noexcept
{
    std::size_t written = 0;
    while (written < text.size())
    {
        // NOLINTNEXTLINE(misc-include-cleaner) - ssize_t is from <sys/types.h>, which clang-tidy 23 doesn't map
        const ssize_t now = ::write(fd, text.data() + written, text.size() - written);
        if (now < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(now);
    }
    return true;
}

} // namespace

std::string replace(const std::filesystem::path& path, std::string_view text)
{
    // The permissions to keep, when there is a regular file to replace. stat() follows a link at @p path, so a link's
    // target's permissions are kept; the rename then replaces the link itself, not the file it named, as the header
    // says (#4434).
    struct stat old{};
    const bool replacing = ::stat(path.c_str(), &old) == 0 && S_ISREG(old.st_mode);

    std::random_device random;
    std::filesystem::path staging;
    int fd = -1;
    for (int attempt = 0; attempt < NAME_ATTEMPTS && fd < 0; ++attempt)
    {
        staging = stagingName(path, random);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic by definition
        // 0644 less the umask: readable by the desktop and other users, as a desktop entry is, never writable by them.
        fd = ::open(staging.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (fd < 0 && errno != EEXIST)
        {
            return std::format("couldn't create {}: {}", staging.string(), errnoText(errno));
        }
    }
    if (fd < 0)
    {
        return std::format("couldn't create a staging file beside {}: every name tried was taken", path.string());
    }
    const auto fail = [&staging](std::string_view what, int error)
    {
        ::unlink(staging.c_str());
        return std::format("couldn't {} {}: {}", what, staging.string(), errnoText(error));
    };
    {
        // Closed at the end of this block, before the rename: once fsync() has succeeded the data is on disk.
        const Posix::FdGuard file(fd);
        if (replacing && ::fchmod(file.get(), old.st_mode & 07777) != 0)
        {
            return fail("set the permissions of", errno);
        }
        if (!writeAll(file.get(), text))
        {
            return fail("write", errno);
        }
        if (::fsync(file.get()) != 0)
        {
            return fail("flush", errno);
        }
    }
    std::error_code ec;
    std::filesystem::rename(staging, path, ec);
    if (ec)
    {
        ::unlink(staging.c_str());
        return std::format("couldn't rename {} over {}: {}", staging.string(), path.string(), ec.message());
    }
    return {};
}

} // namespace Platform::ReplaceFile
