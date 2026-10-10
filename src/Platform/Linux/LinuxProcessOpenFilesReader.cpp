#include "LinuxProcessOpenFilesReader.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessOpenFiles.h"
#include "PosixGuards.h"
#include "ProcFdParser.h"
#include "ProcParsing.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace Platform
{

namespace
{

using Posix::DirGuard;
using Posix::FdGuard;

[[nodiscard]] OpenFilesReadResult failure(int err)
{
    OpenFilesReadResult result;
    switch (err)
    {
    case EACCES:
    case EPERM:
        result.status = OpenFilesReadStatus::PermissionDenied;
        break;
    case ENOENT:
    case ESRCH:
        result.status = OpenFilesReadStatus::ProcessExited;
        break;
    default:
        result.status = OpenFilesReadStatus::Failed;
        result.detail = std::system_category().message(err);
        break;
    }
    return result;
}

/// The start time (/proc/[pid]/stat field 22) of the process whose directory @p pidDirFd is open on;
/// 0 if it can't be read (the process exited).
[[nodiscard]] std::uint64_t startTimeTicksAt(int pidDirFd) noexcept
{
    std::array<char, 1024> buf{};
    const std::size_t len = ProcParsing::readProcFileOnceAt(pidDirFd, "stat", buf.data(), buf.size());
    return ProcParsing::parseStatStartTime(std::string_view(buf.data(), len)).value_or(0);
}

/// The open flags in fdinfo/<name>, if they can be read (the descriptor may have closed since).
[[nodiscard]] std::optional<std::uint32_t> readFlagsAt(int fdinfoDirFd, const char* name) noexcept
{
    if (fdinfoDirFd < 0)
    {
        return std::nullopt;
    }
    std::array<char, 512> buf{}; // pos and flags come first; the rest is not needed
    const std::size_t len = ProcParsing::readProcFileOnceAt(fdinfoDirFd, name, buf.data(), buf.size());
    return ProcFd::parseFdInfoFlags(std::string_view(buf.data(), len));
}

} // namespace

bool LinuxProcessOpenFilesReader::hasOpenFiles() const
{
    return true;
}

OpenFilesReadResult LinuxProcessOpenFilesReader::readOpenFiles(const ProcessTarget& target)
{
    if (target.pid <= 0)
    {
        return failure(ESRCH);
    }
    if (target.startTimeTicks == 0)
    {
        OpenFilesReadResult unknown;
        unknown.status = OpenFilesReadStatus::IdentityUnknown;
        return unknown;
    }

    // One handle on the process's /proc directory: once open it keeps naming this process.
    const std::string dirPath = m_ProcRoot + "/" + std::to_string(target.pid);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const FdGuard dir(::open(dirPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (dir.get() < 0)
    {
        return failure(errno);
    }
    if (startTimeTicksAt(dir.get()) != target.startTimeTicks)
    {
        return failure(ESRCH); // gone, or the PID now names another process
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const FdGuard fdDir(::openat(dir.get(), "fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fdDir.get() < 0)
    {
        return failure(errno); // EACCES: another user's process
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const FdGuard fdinfoDir(::openat(dir.get(), "fdinfo", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    // A second descriptor for the listing: fdopendir() takes it over and the DirGuard closes it.
    const int listFd = ::dup(fdDir.get());
    const DirGuard listing(listFd >= 0 ? ::fdopendir(listFd) : nullptr);
    if (listing.get() == nullptr)
    {
        const int err = errno;
        if (listFd >= 0)
        {
            ::close(listFd);
        }
        return failure(err);
    }

    OpenFilesReadResult result;
    result.status = OpenFilesReadStatus::Ok;
    int deniedLinks = 0; // listing needs only DAC rights; reading a link also needs ptrace access
    std::array<char, 4096> link{};
    for (;;)
    {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) - readdir is safe here: one DIR* per call
        const dirent* entry = ::readdir(listing.get());
        if (entry == nullptr)
        {
            break;
        }
        const std::optional<std::uint64_t> fd = ProcFd::parseFdName(entry->d_name);
        if (!fd.has_value())
        {
            continue; // "." and ".."
        }
        if (result.files.size() >= MAX_OPEN_FILES)
        {
            result.truncated = true;
            break;
        }
        const auto length = ::readlinkat(fdDir.get(), entry->d_name, link.data(), link.size());
        if (length < 0)
        {
            deniedLinks += (errno == EACCES || errno == EPERM) ? 1 : 0;
            continue; // closed since the listing, or denied
        }
        if (length == 0)
        {
            continue;
        }
        const std::string_view text(link.data(), std::min(static_cast<std::size_t>(length), link.size()));
        result.files.push_back(ProcFd::classifyFdLink(*fd, text, readFlagsAt(fdinfoDir.get(), entry->d_name)));
    }
    if (result.files.empty())
    {
        if (deniedLinks > 0)
        {
            return failure(EACCES);
        }
        // An exiting process's fd/ lists empty: confirmed before that is reported as "no open files".
        if (startTimeTicksAt(dir.get()) != target.startTimeTicks)
        {
            return failure(ESRCH);
        }
    }
    std::ranges::sort(result.files, {}, &OpenFile::descriptor);
    return result;
}

} // namespace Platform
