#include "LinuxProcessEnvironmentReader.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/ProcessEnvironment.h"
#include "PosixGuards.h"
#include "ProcParsing.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace Platform
{

namespace
{

using Posix::FdGuard;

/// The whole of the file @p name in the directory @p dirFd, or the errno of the open or read that
/// failed. Read to EOF in chunks: environ is served a page at a time.
[[nodiscard]] std::expected<std::string, int> readWholeFileAt(int dirFd, const char* name)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const FdGuard fd(::openat(dirFd, name, O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0)
    {
        return std::unexpected(errno);
    }
    std::string contents;
    std::array<char, 4096> chunk{};
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

    if (target.startTimeTicks != 0)
    {
        const std::expected<std::string, int> stat = readWholeFileAt(dir.get(), "stat");
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
    }

    const std::expected<std::string, int> block = readWholeFileAt(dir.get(), "environ");
    if (!block.has_value())
    {
        return failure(block.error());
    }
    // Never logged, at any level: the values are routinely secrets (#179).
    return {.status = EnvironmentReadStatus::Ok, .variables = Environment::parseEnvironBlock(*block)};
}

} // namespace Platform
