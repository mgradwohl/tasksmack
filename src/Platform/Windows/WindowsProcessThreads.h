#pragma once

// Windows IProcessThreadsReader (#3069): Process Details' Threads section. One
// NtQuerySystemInformation(SystemProcessInformation) snapshot gives the process's create time (checked
// against the target's, so a reused PID reads as exited) and each of its threads' ID, scheduler state, wait
// reason, priority, CPU times and create time, with no handle on the process: so it lists another user's,
// an elevated or a protected process's threads as well as the user's own, and never needs the helper.
// Each thread is then opened with THREAD_QUERY_LIMITED_INFORMATION and, once GetProcessIdOfThread and
// GetThreadTimes say it is still the thread the snapshot listed, read for its description (GetThreadDescription;
// an empty one is no name) and its suspend count (ThreadSuspendCount); a thread that refuses the open, or whose TID
// now names another thread, keeps its row without them.
//
// The calls it makes are a table, the system's by default, so tests feed it snapshots built in the test
// and fake thread handles (test_WindowsProcessThreads.cpp). Not thread-safe: one read at a time, as the
// Threads view runs them on its worker.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include "Platform/IProcessActions.h"
#include "Platform/IProcessThreads.h"
#include "WindowsNtQuery.h"

#include <cstddef>
#include <vector>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// The calls WindowsProcessThreadsReader makes: ntdll's and kernel32's, or a test's fakes.
struct ProcessThreadFunctions
{
    using OpenThreadFn = HANDLE(WINAPI*)(DWORD access, BOOL inherit, DWORD tid);
    using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
    /// GetThreadDescription (Windows 10 1607 on): the description in a LocalAlloc'd string.
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE thread, PWSTR* description);
    using LocalFreeFn = HLOCAL(WINAPI*)(HLOCAL);
    /// NtQueryInformationThread, its class passed as a ULONG; the result is an NTSTATUS.
    using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE thread, ULONG infoClass, PVOID info, ULONG length, PULONG returned);

    NtQuerySystemInformationFn ntQuerySystemInformation = nullptr;
    OpenThreadFn openThread = nullptr;
    CloseHandleFn closeHandle = nullptr;
    GetThreadDescriptionFn getThreadDescription = nullptr; ///< Null: no names
    LocalFreeFn localFree = nullptr;
    NtQueryInformationThreadFn ntQueryInformationThread = nullptr; ///< Null: no suspend counts
    using GetProcessIdOfThreadFn = DWORD(WINAPI*)(HANDLE thread);
    using GetThreadTimesFn = BOOL(WINAPI*)(HANDLE thread, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user);
    /// The thread opened is the one the snapshot listed: the process's, created when the snapshot says. Null,
    /// either: no names or suspend counts, since no thread can be confirmed.
    GetProcessIdOfThreadFn getProcessIdOfThread = nullptr;
    GetThreadTimesFn getThreadTimes = nullptr;
};

/// The real calls, the optional ones looked up once.
[[nodiscard]] ProcessThreadFunctions realProcessThreadFunctions() noexcept;

// NOLINTEND(misc-include-cleaner)

class WindowsProcessThreadsReader final : public IProcessThreadsReader
{
  public:
    /// The most threads one read lists, as on Linux; past it the read says it was cut.
    static constexpr std::size_t MAX_THREADS = 4096;

    WindowsProcessThreadsReader();
    /// Test seam: every call through @p api.
    explicit WindowsProcessThreadsReader(const ProcessThreadFunctions& api);

    [[nodiscard]] bool hasThreads() const override;
    [[nodiscard]] ThreadsReadResult readThreads(const ProcessTarget& target) override;

  private:
    /// Fills @p thread's name and suspend count from a handle on it, when it can be opened and is still the thread
    /// the snapshot listed: @p pid's, created at its createTime (a TID reused since is left without them).
    void readThreadDetails(ProcessThread& thread, DWORD pid) const;

    ProcessThreadFunctions m_Api;
    std::vector<std::byte> m_Snapshot; // reused between reads: a few hundred KiB, grown as the system grows
};

} // namespace Platform::Windows

#endif // _WIN32
