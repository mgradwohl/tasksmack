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

[[nodiscard]] EnvironmentReadResult environmentFailure(int err)
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
        return environmentFailure(ESRCH);
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
        return environmentFailure(errno);
    }

    const std::expected<std::string, int> stat = ProcParsing::readWholeFileAt(dir.get(), "stat");
    if (!stat.has_value())
    {
        return environmentFailure(stat.error());
    }
    const std::optional<std::uint64_t> startTicks = ProcParsing::parseStatStartTime(*stat);
    if (!startTicks.has_value())
    {
        return environmentFailure(EIO);
    }
    if (*startTicks != target.startTimeTicks)
    {
        return environmentFailure(ESRCH); // the PID now belongs to a different process
    }

    // Capped as the Windows reader is (#4386): the process sizes its own environ, and the kernel caps nothing.
    std::expected<std::string, int> block = ProcParsing::readWholeFileAt(dir.get(), "environ", ENVIRON_READ_CAP_BYTES + 1);
    if (!block.has_value())
    {
        return environmentFailure(block.error());
    }
    bool truncated = block->size() > ENVIRON_READ_CAP_BYTES;
    if (truncated)
    {
        // Up to the last whole entry within the cap, so no variable is shown cut. An entry whose NUL is the byte just
        // past the cap still fits: its text is within it.
        const std::size_t lastEnd = block->rfind('\0', ENVIRON_READ_CAP_BYTES);
        block->resize(lastEnd == std::string::npos ? 0 : lastEnd + 1);
    }
    // And at most ENVIRON_VARIABLE_CAP entries, cut before parsing so the rest are never built (the security review).
    std::size_t entries = 0;
    for (std::size_t at = 0; at < block->size(); ++at)
    {
        if ((*block)[at] == '\0' && ++entries == ENVIRON_VARIABLE_CAP)
        {
            if (at + 1 < block->size())
            {
                block->resize(at + 1);
                truncated = true;
            }
            break;
        }
    }
    // Never logged, at any level: the values are routinely secrets (#179).
    return {.status = EnvironmentReadStatus::Ok, .variables = Environment::parseEnvironBlock(*block), .truncated = truncated};
}

} // namespace Platform
