#include "LinuxProcessModulesReader.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "PosixGuards.h"
#include "ProcMapsParser.h"
#include "ProcParsing.h"

#include <cerrno>
#include <cstddef>
#include <expected>
#include <string>
#include <system_error>

#include <fcntl.h>

namespace Platform
{

namespace
{

using Posix::FdGuard;

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
    if (ProcParsing::startTimeTicksAt(dir.get()) != target.startTimeTicks)
    {
        return failure(ESRCH); // gone, or the PID now names another process
    }
    const std::expected<std::string, int> maps = ProcParsing::readWholeFileAt(dir.get(), "maps");
    if (!maps.has_value())
    {
        return failure(maps.error());
    }
    // An exiting process's maps reads empty: confirmed before that is reported as "no modules".
    if (maps->empty() && ProcParsing::startTimeTicksAt(dir.get()) != target.startTimeTicks)
    {
        return failure(ESRCH);
    }
    return {.status = ModulesReadStatus::Ok, .modules = ProcMaps::parseProcMaps(*maps), .detail = {}};
}

} // namespace Platform
