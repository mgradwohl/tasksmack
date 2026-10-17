#pragma once

// One process's PSS and USS from /proc/[pid]/smaps_rollup (#1885), checked against its start time, and
// why there are none (#2309): shared by LinuxProcessProbe, as itself, and tasksmack-helper, as root, which
// reads another user's process for it.

#include "Platform/IProcessProbe.h"
#include "PosixGuards.h"
#include "ProcParsing.h"
#include "SmapsRollupParser.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>

namespace Platform::Linux
{

/// The PSS and USS of process @p pid started at @p startTimeTicks, read under @p procRoot ("/proc").
/// PermissionDenied when smaps_rollup refuses to open (another user's process without ptrace read
/// access); Unavailable when the process is gone, its PID names another process now, or the file
/// doesn't parse.
[[nodiscard]] inline MemoryRollupRead
readMemoryRollupAt(const std::filesystem::path& procRoot, std::int32_t pid, std::uint64_t startTimeTicks)
{
    if (pid <= 0)
    {
        return std::unexpected(MemoryRollupFailure::Unavailable);
    }
    // One open of the process's directory for both reads: the descriptor stays bound to the process it
    // was opened for, so once stat confirms that is the expected one (not a later process given the
    // same PID), smaps_rollup read through it can't be another's -- a process gone since reads empty.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const Posix::FdGuard pidDirFd(::open((procRoot / std::to_string(pid)).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (pidDirFd.get() < 0)
    {
        return std::unexpected(MemoryRollupFailure::Unavailable);
    }
    std::array<char, 1024> stat{}; // As parseProcessStat(): ample for one stat line
    const std::size_t statLength = ProcParsing::readProcFileOnceAt(pidDirFd.get(), "stat", stat.data(), stat.size());
    if (const std::optional<std::uint64_t> startTime = ProcParsing::parseStatStartTime(std::string_view(stat.data(), statLength));
        !startTime.has_value() || *startTime != startTimeTicks)
    {
        return std::unexpected(MemoryRollupFailure::Unavailable);
    }
    const std::vector<char> text = ProcParsing::readProcFileFullAt(pidDirFd.get(), "smaps_rollup");
    if (text.empty())
    {
        // Why: the kernel refuses another user's smaps_rollup at open() without ptrace read access.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
        const Posix::FdGuard probe(::openat(pidDirFd.get(), "smaps_rollup", O_RDONLY | O_CLOEXEC));
        const bool refused = probe.get() < 0 && (errno == EACCES || errno == EPERM);
        return std::unexpected(refused ? MemoryRollupFailure::PermissionDenied : MemoryRollupFailure::Unavailable);
    }
    const std::optional<MemoryRollup> rollup = SmapsRollup::parse(std::string_view(text.data(), text.size()));
    if (!rollup.has_value())
    {
        return std::unexpected(MemoryRollupFailure::Unavailable);
    }
    return *rollup;
}

} // namespace Platform::Linux
