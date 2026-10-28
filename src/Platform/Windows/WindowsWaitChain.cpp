#include "WindowsWaitChain.h"

#include "Platform/IProcessActions.h"
#include "Platform/WaitChain.h"
#include "WinString.h"
#include "WindowsHandles.h"
#include "WindowsProcAddress.h"
#include "WindowsProcessIdentity.h"
#include "WindowsWaitChainMath.h"

#include <spdlog/spdlog.h>

// NOLINTBEGIN(misc-include-cleaner) - Windows umbrella headers; symbols come from their sub-headers
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <wct.h>
// clang-format on
// NOLINTEND(misc-include-cleaner)

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

// WindowsWaitChainMath.h mirrors wct.h so it can be built and tested anywhere: each value checked here.
static_assert(WaitChainMath::WCT_CRITICAL_SECTION_TYPE == WctCriticalSectionType);
static_assert(WaitChainMath::WCT_SEND_MESSAGE_TYPE == WctSendMessageType);
static_assert(WaitChainMath::WCT_MUTEX_TYPE == WctMutexType);
static_assert(WaitChainMath::WCT_ALPC_TYPE == WctAlpcType);
static_assert(WaitChainMath::WCT_COM_TYPE == WctComType);
static_assert(WaitChainMath::WCT_THREAD_WAIT_TYPE == WctThreadWaitType);
static_assert(WaitChainMath::WCT_PROCESS_WAIT_TYPE == WctProcessWaitType);
static_assert(WaitChainMath::WCT_THREAD_TYPE == WctThreadType);
static_assert(WaitChainMath::WCT_COM_ACTIVATION_TYPE == WctComActivationType);
static_assert(WaitChainMath::WCT_UNKNOWN_TYPE == WctUnknownType);
static_assert(WaitChainMath::WCT_SOCKET_IO_TYPE == WctSocketIoType);
static_assert(WaitChainMath::WCT_SMB_IO_TYPE == WctSmbIoType);
static_assert(WaitChainMath::WCT_STATUS_NO_ACCESS == WctStatusNoAccess);
static_assert(WaitChainMath::WCT_STATUS_RUNNING == WctStatusRunning);
static_assert(WaitChainMath::WCT_STATUS_BLOCKED == WctStatusBlocked);
static_assert(WaitChainMath::WCT_STATUS_PID_ONLY == WctStatusPidOnly);
static_assert(WaitChainMath::WCT_STATUS_PID_ONLY_RPCSS == WctStatusPidOnlyRpcss);
static_assert(WaitChainMath::WCT_STATUS_OWNED == WctStatusOwned);
static_assert(WaitChainMath::WCT_STATUS_NOT_OWNED == WctStatusNotOwned);
static_assert(WaitChainMath::WCT_STATUS_ABANDONED == WctStatusAbandoned);
static_assert(WaitChainMath::WCT_STATUS_UNKNOWN == WctStatusUnknown);
static_assert(WaitChainMath::WCT_STATUS_ERROR == WctStatusError);
static_assert(WaitChainMath::OUT_OF_PROC_FLAG == WCT_OUT_OF_PROC_FLAG);
static_assert(WaitChainMath::OUT_OF_PROC_COM_FLAG == WCT_OUT_OF_PROC_COM_FLAG);
static_assert(WaitChainMath::OUT_OF_PROC_CS_FLAG == WCT_OUT_OF_PROC_CS_FLAG);
static_assert(WaitChainMath::NETWORK_IO_FLAG == WCT_NETWORK_IO_FLAG);
static_assert(WaitChainMath::GETINFO_ALL_FLAGS == WCTP_GETINFO_ALL_FLAGS);
static_assert(WaitChainMath::MAX_NODE_COUNT == WCT_MAX_NODE_COUNT);
static_assert(WaitChainMath::OBJNAME_LENGTH == WCT_OBJNAME_LENGTH);

namespace
{

/// Closes the read's WCT session through the table's CloseThreadWaitChainSession, however the read ends.
class SessionGuard
{
  public:
    SessionGuard(HWCT session, WaitChainFunctions::CloseSessionFn close) noexcept : m_Session(session), m_Close(close)
    {}
    ~SessionGuard()
    {
        m_Close(m_Session);
    }
    SessionGuard(const SessionGuard&) = delete;
    SessionGuard& operator=(const SessionGuard&) = delete;
    SessionGuard(SessionGuard&&) = delete;
    SessionGuard& operator=(SessionGuard&&) = delete;

  private:
    HWCT m_Session;
    WaitChainFunctions::CloseSessionFn m_Close;
};

[[nodiscard]] std::string systemMessage(DWORD error)
{
    return std::system_category().message(static_cast<int>(error));
}

/// Process @p pid's start time in FILETIME ticks (ProcessTarget::startTimeTicks), read now; 0 when it
/// can't be opened (another user's or a protected process), so End process isn't offered for it.
[[nodiscard]] std::uint64_t processStartTime(const ProcessIdentityFunctions& fns, DWORD pid)
{
    HANDLE opened = fns.openProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (opened == nullptr)
    {
        return 0;
    }
    const InjectableHandle process(opened, InjectedHandleCloser{.close = fns.closeHandle});
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (fns.getProcessTimes(process.get(), &creation, &exitTime, &kernelTime, &userTime) == 0)
    {
        return 0;
    }
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime;
}

/// @p info copied out of the OS structure: the union member its type uses.
[[nodiscard]] WaitChainMath::RawWaitNode copyNode(const WAITCHAIN_NODE_INFO& info)
{
    WaitChainMath::RawWaitNode raw;
    raw.objectType = static_cast<std::uint32_t>(info.ObjectType);
    raw.objectStatus = static_cast<std::uint32_t>(info.ObjectStatus);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - WAITCHAIN_NODE_INFO's union, read by the member its type names
    if (raw.objectType == WaitChainMath::WCT_THREAD_TYPE)
    {
        raw.processId = info.ThreadObject.ProcessId;
        raw.threadId = info.ThreadObject.ThreadId;
        raw.waitTime = info.ThreadObject.WaitTime;
        raw.contextSwitches = info.ThreadObject.ContextSwitches;
    }
    else
    {
        // ObjectName need not end in a NUL when it fills the array: never read past it.
        const std::span<const WCHAR> name(info.LockObject.ObjectName);
        const auto end = std::ranges::find(name, L'\0');
        raw.objectName = WinString::wideToUtf8(std::wstring_view(name.data(), static_cast<std::size_t>(end - name.begin())));
    }
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)
    return raw;
}

/// Names each thread node's process from @p listing, and reads its start time once per process (the
/// target's is known), so End process can name exactly that process.
void nameProcesses(std::vector<ThreadWaitChain>& chains,
                   const ThreadListing& listing,
                   const ProcessIdentityFunctions& fns,
                   const ProcessTarget& target)
{
    std::unordered_map<std::int32_t, std::uint64_t> startTimes{{target.pid, target.startTimeTicks}};
    for (ThreadWaitChain& chain : chains)
    {
        for (WaitChainNode& node : chain.nodes)
        {
            if (node.type != WaitObjectType::Thread || node.pid <= 0)
            {
                continue;
            }
            if (const auto name =
                    std::ranges::find(listing.processNames, static_cast<DWORD>(node.pid), &std::pair<DWORD, std::string>::first);
                name != listing.processNames.end())
            {
                node.processName = name->second;
            }
            const auto [known, inserted] = startTimes.try_emplace(node.pid, 0);
            if (inserted)
            {
                known->second = processStartTime(fns, static_cast<DWORD>(node.pid));
            }
            node.processStartTimeTicks = known->second;
        }
    }
}

} // namespace

std::optional<ThreadListing> listProcessThreads(DWORD pid)
{
    const UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD | TH32CS_SNAPPROCESS, 0));
    if (!snapshot)
    {
        return std::nullopt;
    }
    ThreadListing listing;
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    for (BOOL more = Process32FirstW(snapshot.get(), &process); more != FALSE; more = Process32NextW(snapshot.get(), &process))
    {
        const std::span<const WCHAR> exe(process.szExeFile);
        const auto end = std::ranges::find(exe, L'\0');
        listing.processNames.emplace_back(
            process.th32ProcessID, WinString::wideToUtf8(std::wstring_view(exe.data(), static_cast<std::size_t>(end - exe.begin()))));
        process.dwSize = sizeof(process);
    }
    THREADENTRY32 thread{};
    thread.dwSize = sizeof(thread);
    for (BOOL more = Thread32First(snapshot.get(), &thread); more != FALSE; more = Thread32Next(snapshot.get(), &thread))
    {
        if (thread.th32OwnerProcessID == pid)
        {
            listing.threadIds.push_back(thread.th32ThreadID);
        }
        thread.dwSize = sizeof(thread);
    }
    return listing;
}

bool registerWaitChainComCallbacks()
{
    // Once per process: WCT keeps the callbacks for good, and ole32 stays loaded for them (it is
    // already, in any process that has used COM, as TaskSmack has).
    static const bool registered = []
    {
        HMODULE ole32 = GetModuleHandleW(L"ole32.dll");
        if (ole32 == nullptr)
        {
            ole32 = LoadLibraryExW(L"ole32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        const auto callState = getProcAddress<PCOGETCALLSTATE>(ole32, "CoGetCallState");
        const auto activationState = getProcAddress<PCOGETACTIVATIONSTATE>(ole32, "CoGetActivationState");
        if (callState == nullptr || activationState == nullptr)
        {
            spdlog::warn("Wait chains: ole32's CoGetCallState/CoGetActivationState not found; COM calls won't be followed");
            return false;
        }
        RegisterWaitChainCOMCallback(callState, activationState);
        return true;
    }();
    return registered;
}

std::chrono::steady_clock::time_point waitChainClockNow()
{
    return std::chrono::steady_clock::now();
}

ThreadWaitChain decodeWaitChainNodes(DWORD tid, std::span<const WAITCHAIN_NODE_INFO> nodes, bool isCycle, bool truncated)
{
    std::vector<WaitChainMath::RawWaitNode> raw;
    raw.reserve(nodes.size());
    for (const WAITCHAIN_NODE_INFO& node : nodes)
    {
        raw.push_back(copyNode(node));
    }
    return WaitChainMath::decodeChain(WaitChainMath::toId(tid), raw, isCycle, truncated);
}

WaitChainReadResult readWaitChains(const WaitChainFunctions& fns, const ProcessTarget& target, const std::stop_token& stopToken)
{
    WaitChainReadResult result;
    // Held for the whole read: the process checked is the process read, and its PID can't be reused meanwhile.
    const auto verified = openProcessVerified(fns.identity, target, PROCESS_QUERY_LIMITED_INFORMATION);
    if (!verified.has_value())
    {
        const auto failure = toReadFailure<WaitChainReadStatus>(verified.error());
        result.status = failure.status;
        result.detail = failure.detail;
        return result;
    }
    const std::optional<ThreadListing> listing = fns.listThreads(static_cast<DWORD>(target.pid));
    if (!listing.has_value())
    {
        result.status = WaitChainReadStatus::Failed;
        result.detail = "Couldn't list the process's threads: " + systemMessage(GetLastError());
        return result;
    }
    result.threadCount = listing->threadIds.size();

    const bool followCom = fns.registerComCallbacks != nullptr && fns.registerComCallbacks();
    HWCT session = fns.openSession(0, nullptr); // 0: synchronous, so each GetThreadWaitChain returns its chain
    if (session == nullptr)
    {
        result.status = WaitChainReadStatus::Failed;
        result.detail = "Couldn't open a wait chain session: " + systemMessage(GetLastError());
        return result;
    }
    const SessionGuard guard(session, fns.closeSession);

    const DWORD flags = WaitChainMath::chainFlags(followCom);
    const auto started = fns.now();
    std::size_t denied = 0;
    result.threads.reserve(std::min(listing->threadIds.size(), WAIT_CHAIN_MAX_THREADS));
    for (const DWORD tid : listing->threadIds)
    {
        if (stopToken.stop_requested())
        {
            result.status = WaitChainReadStatus::Cancelled;
            return result;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(fns.now() - started);
        if (WaitChainMath::budgetSpent(result.threads.size(), static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed.count(), 0))))
        {
            result.threadsTruncated = true;
            break;
        }
        // Zeroed, so a node Windows didn't fill reads as type 0 and ends the chain (decodeChain()).
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - type 0 is how an unfilled node is told apart
        std::array<WAITCHAIN_NODE_INFO, WCT_MAX_NODE_COUNT> nodes{};
        auto count = static_cast<DWORD>(nodes.size());
        BOOL isCycle = FALSE;
        if (fns.getChain(session, 0, flags, tid, &count, nodes.data(), &isCycle) == FALSE)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_MORE_DATA)
            {
                // Longer than WCT_MAX_NODE_COUNT: its start, marked truncated.
                result.threads.push_back(decodeWaitChainNodes(tid, nodes, isCycle != FALSE, true));
                continue;
            }
            denied += error == ERROR_ACCESS_DENIED ? 1U : 0U;
            ThreadWaitChain failed;
            failed.tid = WaitChainMath::toId(tid);
            failed.error = WaitChainMath::chainErrorText(error, systemMessage(error));
            result.threads.push_back(std::move(failed));
            continue;
        }
        const std::size_t filled = std::min<std::size_t>(count, nodes.size());
        result.threads.push_back(decodeWaitChainNodes(tid, std::span(nodes).first(filled), isCycle != FALSE, false));
    }
    nameProcesses(result.threads, *listing, fns.identity, target);
    result.status = WaitChainMath::readStatus(result.threads.size(), denied);
    return result;
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
