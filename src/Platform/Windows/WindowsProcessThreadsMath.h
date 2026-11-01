#pragma once

// The pure parts of Windows' Threads section reader (#3069), free of any call, so they are tested with
// buffers built in the test (test_WindowsProcessThreads.cpp):
//  - finding one process's entry in a NtQuerySystemInformation(SystemProcessInformation) snapshot and
//    reading its SYSTEM_THREAD_INFORMATION records, bounded by the bytes the kernel wrote;
//  - a thread's scheduler state as the state letter the view names (Domain::processStateName()), and its
//    wait reason (KWAIT_REASON) as the kernel names it;
//  - a record as the platform-neutral Platform::ProcessThread, its times in 100 ns units.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
// clang-format on

#include "Platform/IProcessThreads.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h and winternl.h

/// SYSTEM_INFORMATION_CLASS values asked for, as ULONGs (winternl.h names only some).
inline constexpr ULONG SYSTEM_PROCESS_INFORMATION_CLASS = 5;
inline constexpr ULONG SYSTEM_TIME_OF_DAY_INFORMATION_CLASS = 3;
/// THREADINFOCLASS ThreadSuspendCount (Windows 8.1 on): a ULONG, the thread's suspend count.
inline constexpr ULONG THREAD_SUSPEND_COUNT_CLASS = 35;

/// Where SYSTEM_PROCESS_INFORMATION's CreateTime sits: winternl.h hides it in Reserved1, after the
/// private working set (8), the hard fault count (4), the thread high-water mark (4) and the cycle time (8).
inline constexpr std::size_t PROCESS_CREATE_TIME_OFFSET = 32;
static_assert(offsetof(SYSTEM_PROCESS_INFORMATION, Reserved1) == 8 && sizeof(SYSTEM_PROCESS_INFORMATION::Reserved1) == 48,
              "SYSTEM_PROCESS_INFORMATION's Reserved1 holds the documented fields up to its kernel time");

/// KTHREAD_STATE's values. SYSTEM_THREAD_INFORMATION::ThreadState is a ULONG: kernelThreadState() converts it.
enum class KernelThreadState : std::uint8_t
{
    Initialized = 0,
    Ready = 1,
    Running = 2,
    Standby = 3,
    Terminated = 4,
    Waiting = 5,
    Transition = 6, ///< Ready, but its kernel stack is paged out
    DeferredReady = 7,
    GateWaitObsolete = 8,
    WaitingForProcessInSwap = 9,
};

/// @p threadState (SYSTEM_THREAD_INFORMATION::ThreadState) as a KernelThreadState, or nullopt for one this doesn't know.
[[nodiscard]] constexpr std::optional<KernelThreadState> kernelThreadState(std::uint32_t threadState) noexcept
{
    if (threadState > static_cast<std::uint32_t>(KernelThreadState::WaitingForProcessInSwap))
    {
        return std::nullopt;
    }
    return static_cast<KernelThreadState>(threadState);
}

/// KWAIT_REASON values that mean the thread is suspended.
inline constexpr std::uint32_t WAIT_REASON_SUSPENDED_VALUE = 5;
inline constexpr std::uint32_t WAIT_REASON_WR_SUSPENDED_VALUE = 12;

/// KWAIT_REASON's names, by value, as the kernel (and Process Explorer) names them.
inline constexpr std::array<std::string_view, 43> WAIT_REASON_NAMES{
    "Executive",
    "FreePage",
    "PageIn",
    "PoolAllocation",
    "DelayExecution",
    "Suspended",
    "UserRequest",
    "WrExecutive",
    "WrFreePage",
    "WrPageIn",
    "WrPoolAllocation",
    "WrDelayExecution",
    "WrSuspended",
    "WrUserRequest",
    "WrEventPair",
    "WrQueue",
    "WrLpcReceive",
    "WrLpcReply",
    "WrVirtualMemory",
    "WrPageOut",
    "WrRendezvous",
    "WrKeyedEvent",
    "WrTerminated",
    "WrProcessInSwap",
    "WrCpuRateControl",
    "WrCalloutStack",
    "WrKernel",
    "WrResource",
    "WrPushLock",
    "WrMutex",
    "WrQuantumEnd",
    "WrDispatchInt",
    "WrPreempted",
    "WrYieldExecution",
    "WrFastMutex",
    "WrGuardedMutex",
    "WrRundown",
    "WrAlertByThreadId",
    "WrDeferredPreempt",
    "WrPhysicalFault",
    "WrIoRing",
    "WrMdlCache",
    "WrRcu",
};

/// Whether @p waitReason means a suspended thread.
[[nodiscard]] constexpr bool isSuspendedWaitReason(std::uint32_t waitReason) noexcept
{
    return waitReason == WAIT_REASON_SUSPENDED_VALUE || waitReason == WAIT_REASON_WR_SUSPENDED_VALUE;
}

/// The wait reason's name ("UserRequest"), or "Reason N" for one this table doesn't know.
[[nodiscard]] inline std::string waitReasonName(std::uint32_t waitReason)
{
    if (waitReason < WAIT_REASON_NAMES.size())
    {
        return std::string(WAIT_REASON_NAMES.at(waitReason));
    }
    return std::format("Reason {}", waitReason);
}

/// The state letter for a thread in @p threadState waiting for @p waitReason (when Waiting), as the Windows
/// process probe derives a process's (deriveProcessState(), #1156): R running or ready to run, S waiting, T
/// suspended, X ended, ? a state this doesn't know.
[[nodiscard]] constexpr char threadStateLetter(std::uint32_t threadState, std::uint32_t waitReason) noexcept
{
    const std::optional<KernelThreadState> state = kernelThreadState(threadState);
    if (!state.has_value())
    {
        return '?';
    }
    switch (*state)
    {
    case KernelThreadState::Initialized:
    case KernelThreadState::Ready:
    case KernelThreadState::Running:
    case KernelThreadState::Standby:
    case KernelThreadState::Transition:
    case KernelThreadState::DeferredReady:
        return 'R';
    case KernelThreadState::Waiting:
        return isSuspendedWaitReason(waitReason) ? 'T' : 'S';
    case KernelThreadState::GateWaitObsolete:
    case KernelThreadState::WaitingForProcessInSwap:
        return 'S';
    case KernelThreadState::Terminated:
        return 'X';
    }
    return '?';
}

/// One SYSTEM_THREAD_INFORMATION record, as read from the snapshot.
struct SnapshotThread
{
    std::int32_t tid = 0;
    std::uint32_t state = 0;
    std::uint32_t waitReason = 0;
    std::int32_t priority = 0;    ///< Its current (dynamic) priority, 0 to 31
    std::uint64_t kernelTime = 0; ///< 100 ns units
    std::uint64_t userTime = 0;   ///< 100 ns units
    std::uint64_t createTime = 0; ///< FILETIME: 100 ns units since 1601
};

/// What findProcessThreads() found for one PID.
struct SnapshotProcess
{
    enum class Status : std::uint8_t
    {
        Found,     ///< The PID's entry, its create time and threads
        NotFound,  ///< No entry has the PID: the process has exited
        Malformed, ///< An entry or its thread array runs past the bytes written
    };
    Status status = Status::NotFound;
    std::uint64_t createTime = 0; ///< FILETIME
    std::vector<SnapshotThread> threads;
    bool truncated = false; ///< It had more than the cap; `threads` holds the first
};

/// The PID @p pid's entry in the SystemProcessInformation @p snapshot (only the bytes the kernel wrote), its
/// threads (at most @p maxThreads). Every entry and thread array is bounds-checked: a chain that runs past
/// the bytes ends the walk, Malformed when it was the PID's entry that did.
[[nodiscard]] inline SnapshotProcess findProcessThreads(std::span<const std::byte> snapshot, std::uint32_t pid, std::size_t maxThreads)
{
    SnapshotProcess found;
    std::size_t offset = 0;
    while (offset + sizeof(SYSTEM_PROCESS_INFORMATION) <= snapshot.size())
    {
        SYSTEM_PROCESS_INFORMATION entry{};
        std::memcpy(&entry, snapshot.subspan(offset).data(), sizeof(entry));
        // The kernel keeps PIDs as HANDLE-sized integers; a Windows PID fits in 32 bits.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto entryPid = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(entry.UniqueProcessId));
        if (entryPid == pid)
        {
            const std::size_t entryEnd =
                (entry.NextEntryOffset != 0) ? std::min(snapshot.size(), offset + entry.NextEntryOffset) : snapshot.size();
            const std::size_t threadsBegin = offset + sizeof(SYSTEM_PROCESS_INFORMATION);
            const std::size_t threadsEnd = threadsBegin + (std::size_t{entry.NumberOfThreads} * sizeof(SYSTEM_THREAD_INFORMATION));
            if (threadsEnd > entryEnd)
            {
                found.status = SnapshotProcess::Status::Malformed;
                return found;
            }
            LARGE_INTEGER createTime{};
            std::memcpy(&createTime, snapshot.subspan(offset + PROCESS_CREATE_TIME_OFFSET).data(), sizeof(createTime));
            found.status = SnapshotProcess::Status::Found;
            found.createTime = static_cast<std::uint64_t>(createTime.QuadPart);
            const std::size_t count = std::min<std::size_t>(entry.NumberOfThreads, maxThreads);
            found.truncated = entry.NumberOfThreads > maxThreads;
            found.threads.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                SYSTEM_THREAD_INFORMATION record{};
                std::memcpy(&record, snapshot.subspan(threadsBegin + (i * sizeof(SYSTEM_THREAD_INFORMATION))).data(), sizeof(record));
                // Reserved1 is KernelTime, UserTime and CreateTime, as SYSTEM_THREAD_INFORMATION documents them.
                found.threads.push_back({
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - a TID is a HANDLE-sized integer
                    .tid = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(record.ClientId.UniqueThread)),
                    .state = record.ThreadState,
                    .waitReason = record.WaitReason,
                    .priority = static_cast<std::int32_t>(record.Priority),
                    .kernelTime = static_cast<std::uint64_t>(record.Reserved1[0].QuadPart),
                    .userTime = static_cast<std::uint64_t>(record.Reserved1[1].QuadPart),
                    .createTime = static_cast<std::uint64_t>(record.Reserved1[2].QuadPart),
                });
            }
            return found;
        }
        if (entry.NextEntryOffset == 0)
        {
            break;
        }
        offset += entry.NextEntryOffset; // re-checked against the bytes by the loop
    }
    return found;
}

/// The boot time to count threads' starts from: @p bootFileTime (FILETIME) to the whole second, as Unix
/// seconds, and as a FILETIME. A boot before 1970 (an unset clock) is unknown: both 0.
struct BootTimeBase
{
    std::uint64_t epochSeconds = 0;
    std::uint64_t fileTime = 0;
};

[[nodiscard]] constexpr BootTimeBase bootTimeBase(std::uint64_t bootFileTime) noexcept
{
    constexpr std::uint64_t UNIX_EPOCH_FILETIME = 116444736000000000ULL;
    constexpr std::uint64_t TICKS_PER_SECOND = 10000000ULL;
    if (bootFileTime <= UNIX_EPOCH_FILETIME)
    {
        return {};
    }
    const std::uint64_t seconds = (bootFileTime - UNIX_EPOCH_FILETIME) / TICKS_PER_SECOND;
    return {.epochSeconds = seconds, .fileTime = UNIX_EPOCH_FILETIME + (seconds * TICKS_PER_SECOND)};
}

/// @p thread as the view's row: CPU times in 100 ns units, its start counted from @p boot (0 when it started
/// before it, or the boot time is unknown), its state letter, a waiting thread's wait reason, and its
/// priority in `nice` (the result's windowsPriorities says so). Its name and suspend count are the reader's,
/// from a handle on it.
[[nodiscard]] inline ProcessThread toProcessThread(const SnapshotThread& thread, const BootTimeBase& boot)
{
    ProcessThread row;
    row.tid = thread.tid;
    row.createTime = thread.createTime;
    row.state = threadStateLetter(thread.state, thread.waitReason);
    row.userTicks = thread.userTime;
    row.systemTicks = thread.kernelTime;
    row.nice = thread.priority;
    row.startTicks = (boot.fileTime != 0 && thread.createTime > boot.fileTime) ? thread.createTime - boot.fileTime : 0;
    if (kernelThreadState(thread.state) == KernelThreadState::Waiting)
    {
        row.waitReason = waitReasonName(thread.waitReason);
    }
    return row;
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows

#endif // _WIN32
