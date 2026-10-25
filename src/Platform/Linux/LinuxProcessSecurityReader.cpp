#include "LinuxProcessSecurityReader.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessSecurity.h"
#include "PosixGuards.h"
#include "ProcParsing.h"
#include "ProcStatusSecurityParser.h"
#include "UserNameLookup.h"

#include <cerrno>
#include <cstddef>
#include <expected>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>

namespace Platform
{

namespace
{

using Posix::FdGuard;

/// The most of any one file read here: status is ~1.5 KiB, and a label or a cgroup path is far less.
constexpr std::size_t MAX_FILE_BYTES = std::size_t{64} * 1024;

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
    if (ProcParsing::startTimeTicksAt(dir.get()) != target.startTimeTicks)
    {
        return failure(ESRCH); // gone, or the PID now names another process
    }
    const std::expected<std::string, int> status = ProcParsing::readWholeFileAt(dir.get(), "status", MAX_FILE_BYTES);
    if (!status.has_value())
    {
        return failure(status.error());
    }

    ProcessSecurity security = ProcStatusSecurity::parseStatus(*status);
    if (!security.users.has_value() && ProcParsing::startTimeTicksAt(dir.get()) != target.startTimeTicks)
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
    if (const auto current = ProcParsing::readWholeFileAt(dir.get(), "attr/current", MAX_FILE_BYTES); current.has_value())
    {
        security.securityLabel = ProcStatusSecurity::parseSecurityLabel(*current);
    }
    if (const auto cgroup = ProcParsing::readWholeFileAt(dir.get(), "cgroup", MAX_FILE_BYTES); cgroup.has_value())
    {
        security.controlGroup = ProcStatusSecurity::parseControlGroup(*cgroup);
    }
    return {.status = SecurityReadStatus::Ok, .security = std::move(security), .detail = {}};
}

} // namespace Platform
