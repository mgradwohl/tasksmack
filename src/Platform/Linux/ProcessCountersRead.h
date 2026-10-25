#pragma once

// One process's FD count and I/O bytes for ReadProcessCounters (#2728), checked against its start time:
// what tasksmack-helper, with CAP_DAC_READ_SEARCH and CAP_SYS_PTRACE, reads of another user's process for
// the sampler, whose own read was refused. As MemoryRollupRead.h: one open of the process's directory,
// so once stat confirms it is the expected process, every read through it is that process's.

#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessProbe.h"
#include "PosixGuards.h"
#include "ProcFdScan.h"
#include "ProcIoParser.h"
#include "ProcParsing.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <fcntl.h>

namespace Platform::Linux
{

/// @p target's counters, read under @p procRoot ("/proc"): each figure with whether it was read. nullopt
/// when the process is gone or its PID now names another.
[[nodiscard]] inline std::optional<Broker::ProcessCountersEntry> readProcessCountersAt(const std::filesystem::path& procRoot,
                                                                                       const ProcessTarget& target)
{
    if (target.pid <= 0)
    {
        return std::nullopt;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const Posix::FdGuard pidDirFd(::open((procRoot / std::to_string(target.pid)).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (pidDirFd.get() < 0)
    {
        return std::nullopt;
    }
    std::array<char, 1024> stat{}; // As parseProcessStat(): ample for one stat line
    const std::size_t statLength = ProcParsing::readProcFileOnceAt(pidDirFd.get(), "stat", stat.data(), stat.size());
    if (const std::optional<std::uint64_t> startTime = ProcParsing::parseStatStartTime(std::string_view(stat.data(), statLength));
        !startTime.has_value() || *startTime != target.startTimeTicks)
    {
        return std::nullopt;
    }
    Broker::ProcessCountersEntry entry{.target = target};
    const ProcFdScan::FdScan scan = ProcFdScan::scanFds(pidDirFd.get(), false, [](std::uint64_t) {});
    entry.handleCountRead = scan.listed;
    entry.handleCount = scan.listed ? scan.count : 0;
    std::array<char, 512> io{}; // As LinuxProcessProbe: ample for /proc/[pid]/io
    const std::size_t ioLength = ProcParsing::readProcFileOnceAt(pidDirFd.get(), "io", io.data(), io.size());
    if (const std::optional<ProcIoParser::ProcIo> parsed = ProcIoParser::parse(std::string_view(io.data(), ioLength)))
    {
        entry.ioRead = true;
        entry.readBytes = parsed->readBytes;
        entry.writeBytes = parsed->writeBytes;
    }
    return entry;
}

} // namespace Platform::Linux
