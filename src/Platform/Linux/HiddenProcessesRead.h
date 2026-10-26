#pragma once

// The processes hidepid hides from TaskSmack, for ListHiddenProcesses (#2728): every process under
// procRoot whose PID TaskSmack's own listing didn't find, as the text of a fixed set of its /proc/<pid>
// files -- stat, statm, status, cmdline, io and cgroup, never a path the request names -- read by
// tasksmack-helper, whose CAP_SYS_PTRACE hidepid lets see them. TaskSmack parses the text with its own
// probe's parsers (LinuxProcessProbe::processFromFiles()). As MemoryRollupRead.h: one open of each process's
// directory, so every file read through it is that process's, the one its stat names.

#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessProbe.h"
#include "PosixGuards.h"
#include "ProcParsing.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace Platform::Linux
{

namespace HiddenProcessesDetail
{

/// The file @p name under the /proc/<pid> directory @p pidDirFd, at most MAX_STRING_BYTES of it (what the
/// reply carries); nullopt when it can't be opened or read.
[[nodiscard]] inline std::optional<std::string> readAt(int pidDirFd, const char* name)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const Posix::FdGuard fd(::openat(pidDirFd, name, O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0)
    {
        return std::nullopt;
    }
    std::string text;
    std::array<char, 4096> chunk{};
    while (text.size() < Broker::MAX_STRING_BYTES)
    {
        const auto n = ::read(fd.get(), chunk.data(), std::min(chunk.size(), Broker::MAX_STRING_BYTES - text.size()));
        if (n == 0)
        {
            break;
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return std::nullopt;
        }
        text.append(chunk.data(), static_cast<std::size_t>(n));
    }
    return text;
}

} // namespace HiddenProcessesDetail

/// The processes under @p procRoot ("/proc") whose PIDs aren't in @p visiblePids, each with its files'
/// text; a process whose stat can't be read (it exited) is left out. Past MAX_HIDDEN_PROCESSES the reply
/// says it was cut.
[[nodiscard]] inline Broker::HiddenProcessesReply listHiddenProcessesAt(const std::filesystem::path& procRoot,
                                                                        std::span<const std::int32_t> visiblePids)
{
    std::vector<std::int32_t> visible(visiblePids.begin(), visiblePids.end());
    std::ranges::sort(visible);
    Broker::HiddenProcessesReply reply;
    const Posix::DirGuard procDir(::opendir(procRoot.c_str()));
    if (procDir.get() == nullptr)
    {
        return reply;
    }
    const int procDirFd = ::dirfd(procDir.get());
    while (const dirent* entry = ::readdir(procDir.get())) // NOLINT(concurrency-mt-unsafe) - one DIR* per call
    {
        const std::string_view name(static_cast<const char*>(entry->d_name));
        std::int32_t pid = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), pid);
        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || pid <= 0 || std::ranges::binary_search(visible, pid))
        {
            continue;
        }
        if (reply.processes.size() == Broker::MAX_HIDDEN_PROCESSES)
        {
            reply.truncated = true;
            break;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
        const Posix::FdGuard pidDirFd(::openat(procDirFd, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (pidDirFd.get() < 0)
        {
            continue; // exited since the listing
        }
        std::optional<std::string> stat = HiddenProcessesDetail::readAt(pidDirFd.get(), "stat");
        const std::optional<std::uint64_t> startTime = stat.has_value() ? ProcParsing::parseStatStartTime(*stat) : std::nullopt;
        if (!startTime.has_value())
        {
            continue;
        }
        Broker::HiddenProcess process{.target = {.pid = pid, .startTimeTicks = *startTime}, .stat = std::move(*stat)};
        process.statm = HiddenProcessesDetail::readAt(pidDirFd.get(), "statm").value_or(std::string{});
        process.status = HiddenProcessesDetail::readAt(pidDirFd.get(), "status").value_or(std::string{});
        process.cmdline = HiddenProcessesDetail::readAt(pidDirFd.get(), "cmdline");
        process.io = HiddenProcessesDetail::readAt(pidDirFd.get(), "io").value_or(std::string{});
        process.cgroup = HiddenProcessesDetail::readAt(pidDirFd.get(), "cgroup").value_or(std::string{});
        reply.processes.push_back(std::move(process));
    }
    return reply;
}

} // namespace Platform::Linux
