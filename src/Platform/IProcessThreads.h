#pragma once

// On-demand read of one process's threads (#3069): the Threads section of Process Details. Linux lists
// /proc/[pid]/task; Windows reads the threads NtQuerySystemInformation(SystemProcessInformation) lists
// for the process, and each one's name and suspend count from a handle on it.
//
// Not part of the per-sample enumeration: a process's threads are read only for the process Process
// Details shows, only while its Threads section is open, at the Settings refresh interval. The App
// composition root creates the reader (Platform::makeProcessThreadsReader()) and hands it to the panel,
// which never calls the factory.
//
// The rows are raw: CPU time in clock ticks (100 ns units on Windows), the state letter. CPU %, which needs
// two reads, and the display text are the view's (App/Panels/ProcessThreadsView.h).

#include "Platform/IProcessActions.h"

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace Platform
{

/// What came of one threads read.
enum class ThreadsReadStatus : std::uint8_t
{
    Ok,               ///< Read; `threads` holds them (at least one while the process runs).
    PermissionDenied, ///< The OS refused (another user's process, where /proc is mounted hidepid; never on Windows).
    ProcessExited,    ///< The process is gone, or its PID now belongs to a different process.
    Unsupported,      ///< This run cannot list another process's threads (synthetic runs).
    IdentityUnknown,  ///< The target's start time is unknown (0), so the process could not be confirmed; nothing was read.
    Failed,           ///< Any other error.
};

/// One thread of the process.
struct ProcessThread
{
    std::int32_t tid = 0;
    /// Its name (Linux: comm, at most 15 bytes, a new thread has its process's; Windows: its description,
    /// empty when it has none or it can't be opened)
    std::string name;
    /// The state letter: R running, S sleeping, D waiting on I/O, T stopped, Z zombie... (Windows: derived from
    /// its scheduler state, T for a suspended thread)
    char state = '?';
    std::uint64_t userTicks = 0;   ///< CPU time in user mode, in clock ticks (ticksPerSecond in the result)
    std::uint64_t systemTicks = 0; ///< CPU time in the kernel, in clock ticks
    /// Its nice value (-20 to 19), as Linux schedules each thread by its own; with the result's
    /// windowsPriorities, its Windows priority (0 to 31) instead
    std::int64_t nice = 0;
    std::uint64_t startTicks = 0; ///< When it started, in clock ticks after boot
    /// Windows: what a waiting thread waits for, as the kernel names it ("UserRequest", "Suspended"); empty
    /// when it isn't waiting, and on Linux (#3069)
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::string waitReason{};
    /// Windows: how many suspensions it has that no resume took back; 0 when it has none or that can't be
    /// read, and on Linux (#3069)
    std::uint32_t suspendCount = 0;
    /// Windows: when it was created, as a FILETIME (100 ns since 1601): the identity a thread action checks, so a
    /// TID reused since this read is never acted on (#3069). 0 when unknown, and on Linux.
    std::uint64_t createTime = 0;
};

/// The outcome of IProcessThreadsReader::readThreads(): a status, and the threads when Ok.
struct ThreadsReadResult
{
    ThreadsReadStatus status = ThreadsReadStatus::Unsupported;
    std::vector<ProcessThread> threads;
    std::string detail;                ///< For Failed, what failed, if known; else empty.
    std::int64_t ticksPerSecond = 100; ///< What the tick counts are in (sysconf(_SC_CLK_TCK) on Linux)
    bool truncated = false;            ///< The process had more than MAX_THREADS; `threads` holds the first
    std::uint64_t bootTimeEpoch = 0;   ///< When the machine booted (Unix seconds), for startTicks as a time; 0 when unknown
    bool windowsPriorities = false;    ///< Each thread's nice holds its Windows priority (0 to 31), not a nice value
    /// When the CPU times were read, in seconds on a steady clock, taken on the reading thread (#3337): what a
    /// view's CPU % divides by, never when the UI took the result in. Unset when unknown; App's
    /// ProcessThreadsView stamps it on its worker as the read returns when a reader leaves it unset.
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::optional<double> acquiredAtSeconds{};
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

    /// Whether readAuthorized() can read as an administrator, after a password prompt (Linux: the
    /// pkexec-started helper, #3069). The UI then offers "Authenticate to read" on PermissionDenied.
    [[nodiscard]] virtual bool canReadAuthorized() const
    {
        return false;
    }

    /// Reads @p target's threads as an administrator, asking for the password first if need be (#3069).
    /// Blocks for as long as the prompt is up, so it runs on a worker. The default reads as readThreads() does.
    [[nodiscard]] virtual ThreadsReadResult readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken)
    {
        static_cast<void>(stopToken);
        return readThreads(target);
    }
};

/// The reader for a run that cannot list another process's threads (synthetic scenarios).
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
