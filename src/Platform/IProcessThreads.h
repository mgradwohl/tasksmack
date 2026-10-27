#pragma once

// On-demand read of one process's threads (#3069): the Threads section of Process Details. Linux lists
// /proc/[pid]/task; Windows (the Windows lane's part) would read the Toolhelp thread snapshot.
//
// Not part of the per-sample enumeration: a process's threads are read only for the process Process
// Details shows, only while its Threads section is open, at the Settings refresh interval. The App
// composition root creates the reader (Platform::makeProcessThreadsReader()) and hands it to the panel,
// which never calls the factory.
//
// The rows are raw: CPU time in clock ticks, the state letter. CPU %, which needs two reads, and the
// display text are the view's (App/Panels/ProcessThreadsView.h).

#include "Platform/IProcessActions.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Platform
{

/// What came of one threads read.
enum class ThreadsReadStatus : std::uint8_t
{
    Ok,               ///< Read; `threads` holds them (at least one while the process runs).
    PermissionDenied, ///< The OS refused (another user's process, where /proc is mounted hidepid).
    ProcessExited,    ///< The process is gone, or its PID now belongs to a different process.
    Unsupported,      ///< This run cannot list another process's threads (synthetic runs, Windows for now).
    IdentityUnknown,  ///< The target's start time is unknown (0), so the process could not be confirmed; nothing was read.
    Failed,           ///< Any other error.
};

/// One thread of the process.
struct ProcessThread
{
    std::int32_t tid = 0;
    std::string name;              ///< Its name (Linux: comm, at most 15 bytes; a new thread has its process's)
    char state = '?';              ///< Linux's state letter: R running, S sleeping, D waiting on I/O, T stopped, Z zombie...
    std::uint64_t userTicks = 0;   ///< CPU time in user mode, in clock ticks (ticksPerSecond in the result)
    std::uint64_t systemTicks = 0; ///< CPU time in the kernel, in clock ticks
    std::int64_t nice = 0;         ///< Its nice value (-20 to 19): Linux schedules each thread by its own
    std::uint64_t startTicks = 0;  ///< When it started, in clock ticks after boot
};

/// The outcome of IProcessThreadsReader::readThreads(): a status, and the threads when Ok.
struct ThreadsReadResult
{
    ThreadsReadStatus status = ThreadsReadStatus::Unsupported;
    std::vector<ProcessThread> threads;
    std::string detail;                ///< For Failed, what failed, if known; else empty.
    std::int64_t ticksPerSecond = 100; ///< What the tick counts are in (sysconf(_SC_CLK_TCK) on Linux)
    bool truncated = false;            ///< The process had more than MAX_THREADS; `threads` holds the first
};

/// Lists one process's threads on request.
class IProcessThreadsReader
{
  public:
    virtual ~IProcessThreadsReader() = default;

    IProcessThreadsReader() = default;
    IProcessThreadsReader(const IProcessThreadsReader&) = default;
    IProcessThreadsReader& operator=(const IProcessThreadsReader&) = default;
    IProcessThreadsReader(IProcessThreadsReader&&) = default;
    IProcessThreadsReader& operator=(IProcessThreadsReader&&) = default;

    /// Whether this run can list a process's threads at all. When false the UI hides the Threads
    /// section and readThreads() only ever returns Unsupported.
    [[nodiscard]] virtual bool hasThreads() const = 0;

    /// Read @p target's threads now. Synchronous: the view runs it on a worker. The target's start time
    /// is checked against the process holding the PID, so a reused PID reports ProcessExited; an
    /// unknown start time (0) is refused with IdentityUnknown.
    [[nodiscard]] virtual ThreadsReadResult readThreads(const ProcessTarget& target) = 0;
};

/// The reader for a run that cannot list another process's threads (synthetic scenarios, Windows for now).
class UnsupportedProcessThreadsReader final : public IProcessThreadsReader
{
  public:
    [[nodiscard]] bool hasThreads() const override
    {
        return false;
    }

    [[nodiscard]] ThreadsReadResult readThreads(const ProcessTarget& /*target*/) override
    {
        return {.status = ThreadsReadStatus::Unsupported, .threads = {}, .detail = {}};
    }
};

} // namespace Platform
