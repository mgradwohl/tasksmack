#include "LinuxProcessEnvironmentReader.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/ProcessEnvironment.h"
#include "PosixGuards.h"
#include "ProcParsing.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <fcntl.h>

namespace Platform
{

namespace
{

using Posix::FdGuard;

[[nodiscard]] EnvironmentReadResult failure(int err)
{
    return {.status = Environment::statusFromErrno(err), .variables = {}};
}

} // namespace

bool LinuxProcessEnvironmentReader::hasEnvironment() const
{
    return true;
}

EnvironmentReadResult LinuxProcessEnvironmentReader::readEnvironment(const ProcessTarget& target)
{
    if (target.pid <= 0)
    {
        return failure(ESRCH);
    }
    if (target.startTimeTicks == 0)
    {
        // Unconfirmed identity: refused rather than read by PID alone, as checkProcessIdentity() refuses
        // an action. The caller retries at its next refresh, once the process's start time is known.
        return {.status = EnvironmentReadStatus::IdentityUnknown, .variables = {}};
    }

    // One handle on the process's /proc directory: once open it keeps naming this process, so a file
    // read through it after the process exits fails (ESRCH/ENOENT) instead of reaching whatever
    // process is given the PID next.
    const std::string dirPath = "/proc/" + std::to_string(target.pid);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const FdGuard dir(::open(dirPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (dir.get() < 0)
    {
        return failure(errno);
    }

    const std::expected<std::string, int> stat = ProcParsing::readWholeFileAt(dir.get(), "stat");
    if (!stat.has_value())
    {
        return failure(stat.error());
    }
    const std::optional<std::uint64_t> startTicks = ProcParsing::parseStatStartTime(*stat);
    if (!startTicks.has_value())
    {
        return failure(EIO);
    }
    if (*startTicks != target.startTimeTicks)
    {
        return failure(ESRCH); // the PID now belongs to a different process
    }

    const std::expected<std::string, int> block = ProcParsing::readWholeFileAt(dir.get(), "environ");
    if (!block.has_value())
    {
        return failure(block.error());
    }
    // Never logged, at any level: the values are routinely secrets (#179).
    return {.status = EnvironmentReadStatus::Ok, .variables = Environment::parseEnvironBlock(*block)};
}

} // namespace Platform
