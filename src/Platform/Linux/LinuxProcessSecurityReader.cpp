#include "LinuxProcessSecurityReader.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessSecurity.h"
#include "PosixGuards.h"
#include "ProcParsing.h"
#include "ProcStatusSecurityParser.h"
#include "UserNameLookup.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

namespace Platform
{

namespace
{

using Posix::FdGuard;

/// The most of any one file read here: status is ~1.5 KiB, and a label or a cgroup path is far less.
constexpr std::size_t MAX_FILE_BYTES = std::size_t{64} * 1024;

/// The file @p name in the directory @p dirFd, up to MAX_FILE_BYTES, or the errno of the open or read
/// that failed.
[[nodiscard]] std::expected<std::string, int> readFileAt(int dirFd, const char* name)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const FdGuard fd(::openat(dirFd, name, O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0)
    {
        return std::unexpected(errno);
    }
    std::string contents;
    std::array<char, 4096> chunk{};
    while (contents.size() < MAX_FILE_BYTES)
    {
        const auto n = ::read(fd.get(), chunk.data(), chunk.size());
        if (n == 0)
        {
            break;
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
    return contents;
}

[[nodiscard]] SecurityReadResult failure(int err)
{
    switch (err)
    {
    case EACCES:
    case EPERM:
        return {.status = SecurityReadStatus::PermissionDenied, .security = {}, .detail = {}};
    case ENOENT:
    case ESRCH:
        return {.status = SecurityReadStatus::ProcessExited, .security = {}, .detail = {}};
    default:
        return {.status = SecurityReadStatus::Failed, .security = {}, .detail = std::system_category().message(err)};
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

void nameUser(SecurityPrincipal& principal)
{
    principal.name = lookUpUserName(static_cast<uid_t>(principal.id), ::getpwuid_r).value_or(std::string{});
}

void nameGroup(SecurityPrincipal& principal)
{
    principal.name = lookUpGroupName(static_cast<gid_t>(principal.id), ::getgrgid_r).value_or(std::string{});
}

} // namespace

bool LinuxProcessSecurityReader::hasSecurity() const
{
    return true;
}

SecurityReadResult LinuxProcessSecurityReader::readSecurity(const ProcessTarget& target)
{
    if (target.pid <= 0)
    {
        return failure(ESRCH);
    }
    if (target.startTimeTicks == 0)
    {
        // Unconfirmed identity: refused rather than read by PID alone, as checkProcessIdentity() refuses
        // an action. The caller retries at its next refresh, once the process's start time is known.
        return {.status = SecurityReadStatus::IdentityUnknown, .security = {}, .detail = {}};
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
    const std::expected<std::string, int> status = readFileAt(dir.get(), "status");
    if (!status.has_value())
    {
        return failure(status.error());
    }

    ProcessSecurity security = ProcStatusSecurity::parseStatus(*status);
    if (!security.users.has_value() && startTimeTicksAt(dir.get()) != target.startTimeTicks)
    {
        return failure(ESRCH); // an exiting process's status can read short
    }
    if (security.users.has_value())
    {
        nameUser(security.users->real);
        nameUser(security.users->effective);
        nameUser(security.users->saved);
        nameUser(security.users->filesystem);
    }
    if (security.groups.has_value())
    {
        nameGroup(security.groups->real);
        nameGroup(security.groups->effective);
        nameGroup(security.groups->saved);
        nameGroup(security.groups->filesystem);
    }
    for (SecurityPrincipal& group : security.supplementaryGroups)
    {
        nameGroup(group);
    }

    // Optional extras: a label or cgroup that can't be read is left empty, not a failed read.
    if (const auto current = readFileAt(dir.get(), "attr/current"); current.has_value())
    {
        security.securityLabel = ProcStatusSecurity::parseSecurityLabel(*current);
    }
    if (const auto cgroup = readFileAt(dir.get(), "cgroup"); cgroup.has_value())
    {
        security.controlGroup = ProcStatusSecurity::parseControlGroup(*cgroup);
    }
    return {.status = SecurityReadStatus::Ok, .security = std::move(security), .detail = {}};
}

} // namespace Platform
