#include "WindowsProcessThreads.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessThreads.h"
#include "WinString.h"
#include "WindowsConversions.h"
#include "WindowsHandles.h"
#include "WindowsNtQuery.h"
#include "WindowsProcAddress.h"
#include "WindowsProcessThreadsMath.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers
namespace
{

/// STATUS_INFO_LENGTH_MISMATCH: the snapshot didn't fit (NTSTATUS is signed, hence the cast).
constexpr LONG STATUS_INFO_LENGTH_MISMATCH_VALUE = static_cast<LONG>(0xC0000004L);
constexpr std::size_t SNAPSHOT_INITIAL_BYTES = std::size_t{512} * 1024;
constexpr std::size_t SNAPSHOT_HEADROOM_BYTES = std::size_t{64} * 1024;
/// Tries at a snapshot that fits before giving up: the process list grows between the calls.
constexpr int MAX_SNAPSHOT_ATTEMPTS = 4;

/// Frees a string GetThreadDescription allocated, through the injected LocalFree.
struct LocalStringFree
{
    ProcessThreadFunctions::LocalFreeFn free = nullptr;

    void operator()(wchar_t* text) const noexcept
    {
        free(text);
    }
};

[[nodiscard]] ThreadsReadResult failure(ThreadsReadStatus status, std::string detail = {})
{
    return {.status = status, .threads = {}, .detail = std::move(detail), .ticksPerSecond = 10'000'000};
}

/// SYSTEM_TIMEOFDAY_INFORMATION's start: the boot time, then the time now, each a FILETIME.
struct TimeOfDay
{
    LARGE_INTEGER bootTime;
    LARGE_INTEGER currentTime;
    LARGE_INTEGER timeZoneBias;
    ULONG timeZoneId;
    ULONG reserved;
    ULONGLONG bootTimeBias;
    ULONGLONG sleepTimeBias;
};

} // namespace

ProcessThreadFunctions realProcessThreadFunctions() noexcept
{
    // GetThreadDescription is kernel32's from Windows 10 1607; looked up so an older Windows still lists threads.
    static const auto getDescription = [] noexcept
    {
        return getProcAddress<ProcessThreadFunctions::GetThreadDescriptionFn>(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    }();
    static const auto queryThread = [] noexcept
    {
        return ntdllProc<ProcessThreadFunctions::NtQueryInformationThreadFn>("NtQueryInformationThread");
    }();
    return {.ntQuerySystemInformation = ntQuerySystemInformation(),
            .openThread = &OpenThread,
            .closeHandle = &CloseHandle,
            .getThreadDescription = getDescription,
            .localFree = &LocalFree,
            .ntQueryInformationThread = queryThread,
            .getProcessIdOfThread = &GetProcessIdOfThread,
            .getThreadTimes = &GetThreadTimes};
}

WindowsProcessThreadsReader::WindowsProcessThreadsReader() : WindowsProcessThreadsReader(realProcessThreadFunctions())
{}

WindowsProcessThreadsReader::WindowsProcessThreadsReader(const ProcessThreadFunctions& api) : m_Api(api)
{}

bool WindowsProcessThreadsReader::hasThreads() const
{
    return m_Api.ntQuerySystemInformation != nullptr;
}

ThreadsReadResult WindowsProcessThreadsReader::readThreads(const ProcessTarget& target)
{
    if (m_Api.ntQuerySystemInformation == nullptr)
    {
        return failure(ThreadsReadStatus::Unsupported);
    }
    if (target.startTimeTicks == 0)
    {
        return failure(ThreadsReadStatus::IdentityUnknown); // the System Idle Process, or a process not yet confirmed
    }
    if (target.pid <= 0)
    {
        return failure(ThreadsReadStatus::ProcessExited);
    }

    if (m_Snapshot.empty())
    {
        m_Snapshot.resize(SNAPSHOT_INITIAL_BYTES);
    }
    LONG status = STATUS_INFO_LENGTH_MISMATCH_VALUE;
    ULONG written = 0;
    for (int attempt = 0; attempt < MAX_SNAPSHOT_ATTEMPTS && status == STATUS_INFO_LENGTH_MISMATCH_VALUE; ++attempt)
    {
        written = 0;
        const auto size = static_cast<ULONG>(std::min<std::size_t>(m_Snapshot.size(), std::numeric_limits<ULONG>::max()));
        status = m_Api.ntQuerySystemInformation(SYSTEM_PROCESS_INFORMATION_CLASS, m_Snapshot.data(), size, &written);
        if (status == STATUS_INFO_LENGTH_MISMATCH_VALUE)
        {
            m_Snapshot.resize(std::max<std::size_t>(m_Snapshot.size(), written) + SNAPSHOT_HEADROOM_BYTES);
        }
    }
    if (status < 0)
    {
        return failure(ThreadsReadStatus::Failed,
                       std::format("NtQuerySystemInformation failed: NTSTATUS 0x{:08X}", static_cast<std::uint32_t>(status)));
    }
    // The CPU times are the snapshot's: stamped now, before the per-thread opens below, whose time varies with
    // the thread count, so it never stretches or shrinks the interval a CPU % divides by (#3337).
    const double acquiredAt = threadsReadClockSeconds();
    const std::size_t bytes = std::min<std::size_t>(m_Snapshot.size(), written != 0 ? written : m_Snapshot.size());
    const SnapshotProcess process =
        findProcessThreads(std::span<const std::byte>(m_Snapshot).first(bytes), static_cast<std::uint32_t>(target.pid), MAX_THREADS);
    switch (process.status)
    {
    case SnapshotProcess::Status::NotFound:
        return failure(ThreadsReadStatus::ProcessExited);
    case SnapshotProcess::Status::Malformed:
        return failure(ThreadsReadStatus::Failed, "The system's process list was malformed");
    case SnapshotProcess::Status::Found:
        break;
    }
    if (process.createTime != target.startTimeTicks)
    {
        return failure(ThreadsReadStatus::ProcessExited); // the PID now names a different process
    }

    // The boot time, for each thread's start as a time; unknown (the dash) when it can't be read.
    TimeOfDay timeOfDay{};
    ULONG timeBytes = 0;
    const bool haveBoot =
        m_Api.ntQuerySystemInformation(SYSTEM_TIME_OF_DAY_INFORMATION_CLASS, &timeOfDay, sizeof(timeOfDay), &timeBytes) >= 0 &&
        timeBytes >= sizeof(LARGE_INTEGER);
    const BootTimeBase boot = haveBoot ? bootTimeBase(static_cast<std::uint64_t>(timeOfDay.bootTime.QuadPart)) : BootTimeBase{};

    ThreadsReadResult result = failure(ThreadsReadStatus::Ok);
    result.truncated = process.truncated;
    result.bootTimeEpoch = boot.epochSeconds;
    result.windowsPriorities = true;
    result.acquiredAtSeconds = acquiredAt;
    result.threads.reserve(process.threads.size());
    for (const SnapshotThread& thread : process.threads)
    {
        ProcessThread row = toProcessThread(thread, boot);
        readThreadDetails(row, static_cast<DWORD>(target.pid));
        result.threads.push_back(std::move(row));
    }
    return result;
}

void WindowsProcessThreadsReader::readThreadDetails(ProcessThread& thread, DWORD pid) const
{
    if (m_Api.openThread == nullptr || m_Api.closeHandle == nullptr || m_Api.getProcessIdOfThread == nullptr ||
        m_Api.getThreadTimes == nullptr || thread.tid <= 0)
    {
        return;
    }
    const InjectableHandle handle(m_Api.openThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(thread.tid)),
                                  InjectedHandleCloser{.close = m_Api.closeHandle});
    if (!handle)
    {
        return; // another user's or a protected process's thread: listed without its name
    }
    // The thread the snapshot listed: one that exited since may have left its TID to a new thread, of this process
    // or another, whose name and suspend count would then be shown on the old one's row.
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (m_Api.getProcessIdOfThread(handle.get()) != pid || m_Api.getThreadTimes(handle.get(), &created, &exited, &kernel, &user) == FALSE ||
        fileTimeTicks(created) != thread.createTime)
    {
        return;
    }
    if (m_Api.getThreadDescription != nullptr && m_Api.localFree != nullptr)
    {
        PWSTR raw = nullptr;
        const HRESULT described = m_Api.getThreadDescription(handle.get(), &raw);
        // Owned at once, so the conversion below can throw without leaking the LocalAlloc'd string.
        const std::unique_ptr<wchar_t, LocalStringFree> description(raw, LocalStringFree{.free = m_Api.localFree});
        if (SUCCEEDED(described) && description != nullptr)
        {
            thread.name = WinString::wideToUtf8(std::wstring_view(description.get(), std::wcslen(description.get())));
        }
    }
    if (m_Api.ntQueryInformationThread != nullptr)
    {
        ULONG count = 0;
        if (m_Api.ntQueryInformationThread(handle.get(), THREAD_SUSPEND_COUNT_CLASS, &count, sizeof(count), nullptr) >= 0)
        {
            thread.suspendCount = count;
        }
    }
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
