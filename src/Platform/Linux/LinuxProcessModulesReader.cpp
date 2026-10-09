#include "LinuxProcessModulesReader.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "PosixGuards.h"
#include "ProcMapsParser.h"
#include "ProcParsing.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace Platform
{

namespace
{

using Posix::FdGuard;

/// The whole of the file @p name in the directory @p dirFd, or the errno of the open or read that
/// failed. Read to EOF in chunks: maps is served a page at a time.
[[nodiscard]] std::expected<std::string, int> readWholeFileAt(int dirFd, const char* name)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const FdGuard fd(::openat(dirFd, name, O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0)
    {
        return std::unexpected(errno);
    }
    std::string contents;
    std::array<char, 8192> chunk{};
    for (;;)
    {
        const auto n = ::read(fd.get(), chunk.data(), chunk.size());
        if (n == 0)
        {
            return contents;
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue; // interrupted by a signal: retry
            }
            return std::unexpected(errno);
        }
        contents.append(chunk.data(), static_cast<std::size_t>(n));
    }
}

[[nodiscard]] ModulesReadResult failure(int err)
{
    switch (err)
    {
    case EACCES:
    case EPERM:
        return {.status = ModulesReadStatus::PermissionDenied, .modules = {}, .detail = {}};
    case ENOENT:
    case ESRCH:
        return {.status = ModulesReadStatus::ProcessExited, .modules = {}, .detail = {}};
    default:
        return {.status = ModulesReadStatus::Failed, .modules = {}, .detail = std::system_category().message(err)};
    }
}

/// The start time (/proc/[pid]/stat field 22) of the process whose directory @p pidDirFd is open on;
/// 0 if it can't be read (the process exited).
[[nodiscard]] std::uint64_t startTimeTicksAt(int pidDirFd) noexcept
{
    std::array<char, 1024> buf{};
    const std::size_t len = ProcParsing::readProcFileOnceAt(pidDirFd, "stat", buf.data(), buf.size());
    return ProcParsing::parseStatStartTime(std::string_view(buf.data(), len)).value_or(0);
}

} // namespace

bool LinuxProcessModulesReader::hasModules() const
{
    return true;
}

ModulesReadResult LinuxProcessModulesReader::readModules(const ProcessTarget& target)
{
    if (target.pid <= 0)
    {
        return failure(ESRCH);
    }
    if (target.startTimeTicks == 0)
    {
        // Unconfirmed identity: refused rather than read by PID alone, as checkProcessIdentity() refuses
        // an action. The caller retries at its next refresh, once the process's start time is known.
        return {.status = ModulesReadStatus::IdentityUnknown, .modules = {}, .detail = {}};
    }

    // One handle on the process's /proc directory: once open it keeps naming this process, so a file
    // read through it after the process exits fails instead of reaching the PID's next owner.
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
    const std::expected<std::string, int> maps = readWholeFileAt(dir.get(), "maps");
    if (!maps.has_value())
    {
        return failure(maps.error());
    }
    // An exiting process's maps reads empty: confirmed before that is reported as "no modules".
    if (maps->empty() && startTimeTicksAt(dir.get()) != target.startTimeTicks)
    {
        return failure(ESRCH);
    }
    return {.status = ModulesReadStatus::Ok, .modules = ProcMaps::parseProcMaps(*maps), .detail = {}};
}

} // namespace Platform
