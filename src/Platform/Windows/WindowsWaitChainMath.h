#pragma once

// The pure part of Windows' wait chain read (#3084, WindowsWaitChain.cpp): wct.h's constants, mirrored
// here so this header stays free of <windows.h> (WindowsWaitChain.cpp static_asserts each against the
// real one), and the decoding of GetThreadWaitChain's WAITCHAIN_NODE_INFO array into Platform's
// ThreadWaitChain. WindowsWaitChain.cpp copies each node out of the OS structure into a RawWaitNode
// (the name already UTF-8); everything from there on is here, so it is tested on every platform
// (tests/Platform/WindowsMath/test_WindowsWaitChainMath.cpp), Linux's coverage included.

#include "Platform/WaitChain.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace Platform::WaitChainMath
{

// WCT_OBJECT_TYPE (wct.h).
inline constexpr std::uint32_t WCT_CRITICAL_SECTION_TYPE = 1;
inline constexpr std::uint32_t WCT_SEND_MESSAGE_TYPE = 2;
inline constexpr std::uint32_t WCT_MUTEX_TYPE = 3;
inline constexpr std::uint32_t WCT_ALPC_TYPE = 4;
inline constexpr std::uint32_t WCT_COM_TYPE = 5;
inline constexpr std::uint32_t WCT_THREAD_WAIT_TYPE = 6;
inline constexpr std::uint32_t WCT_PROCESS_WAIT_TYPE = 7;
inline constexpr std::uint32_t WCT_THREAD_TYPE = 8;
inline constexpr std::uint32_t WCT_COM_ACTIVATION_TYPE = 9;
inline constexpr std::uint32_t WCT_UNKNOWN_TYPE = 10;
inline constexpr std::uint32_t WCT_SOCKET_IO_TYPE = 11;
inline constexpr std::uint32_t WCT_SMB_IO_TYPE = 12;

// WCT_OBJECT_STATUS (wct.h).
inline constexpr std::uint32_t WCT_STATUS_NO_ACCESS = 1;
inline constexpr std::uint32_t WCT_STATUS_RUNNING = 2;
inline constexpr std::uint32_t WCT_STATUS_BLOCKED = 3;
inline constexpr std::uint32_t WCT_STATUS_PID_ONLY = 4;
inline constexpr std::uint32_t WCT_STATUS_PID_ONLY_RPCSS = 5;
inline constexpr std::uint32_t WCT_STATUS_OWNED = 6;
inline constexpr std::uint32_t WCT_STATUS_NOT_OWNED = 7;
inline constexpr std::uint32_t WCT_STATUS_ABANDONED = 8;
inline constexpr std::uint32_t WCT_STATUS_UNKNOWN = 9;
inline constexpr std::uint32_t WCT_STATUS_ERROR = 10;

// GetThreadWaitChain's flags (wct.h's WCT_OUT_OF_PROC_FLAG and the rest, named without their prefix here:
// wct.h defines those as macros): follow chains into other processes, through COM, through critical
// sections in other processes, and through network I/O. GETINFO_ALL_FLAGS is wct.h's WCTP_GETINFO_ALL_FLAGS,
// the first three; the network I/O flag came later and isn't in it.
inline constexpr std::uint32_t OUT_OF_PROC_FLAG = 0x1;
inline constexpr std::uint32_t OUT_OF_PROC_COM_FLAG = 0x2;
inline constexpr std::uint32_t OUT_OF_PROC_CS_FLAG = 0x4;
inline constexpr std::uint32_t NETWORK_IO_FLAG = 0x8;
inline constexpr std::uint32_t GETINFO_ALL_FLAGS = OUT_OF_PROC_FLAG | OUT_OF_PROC_COM_FLAG | OUT_OF_PROC_CS_FLAG;

/// WCT_MAX_NODE_COUNT: the most nodes one GetThreadWaitChain call returns.
inline constexpr std::uint32_t MAX_NODE_COUNT = 16;
/// WCT_OBJNAME_LENGTH: the WCHARs in a node's ObjectName, its terminating NUL included when it fits.
inline constexpr std::size_t OBJNAME_LENGTH = 128;

static_assert(MAX_NODE_COUNT == WAIT_CHAIN_MAX_NODES, "a chain is as long as Windows makes one");

/// The flags a read asks GetThreadWaitChain for: every one, network I/O included (so a socket or SMB wait
/// is named), except COM's when the COM callbacks couldn't be registered (RegisterWaitChainCOMCallback),
/// without which Windows can't follow a COM call.
[[nodiscard]] constexpr std::uint32_t chainFlags(bool comCallbacksRegistered) noexcept
{
    constexpr std::uint32_t ALL = GETINFO_ALL_FLAGS | NETWORK_IO_FLAG;
    return comCallbacksRegistered ? ALL : (ALL & ~OUT_OF_PROC_COM_FLAG);
}

/// The WCT_OBJECT_TYPE @p raw as Platform's type; Unknown for WctUnknownType and for any value this
/// build doesn't know.
[[nodiscard]] constexpr WaitObjectType objectType(std::uint32_t raw) noexcept
{
    switch (raw)
    {
    case WCT_CRITICAL_SECTION_TYPE:
        return WaitObjectType::CriticalSection;
    case WCT_SEND_MESSAGE_TYPE:
        return WaitObjectType::SendMessageCall;
    case WCT_MUTEX_TYPE:
        return WaitObjectType::Mutex;
    case WCT_ALPC_TYPE:
        return WaitObjectType::Alpc;
    case WCT_COM_TYPE:
        return WaitObjectType::Com;
    case WCT_THREAD_WAIT_TYPE:
        return WaitObjectType::ThreadWait;
    case WCT_PROCESS_WAIT_TYPE:
        return WaitObjectType::ProcessWait;
    case WCT_THREAD_TYPE:
        return WaitObjectType::Thread;
    case WCT_COM_ACTIVATION_TYPE:
        return WaitObjectType::ComActivation;
    case WCT_SOCKET_IO_TYPE:
        return WaitObjectType::SocketIo;
    case WCT_SMB_IO_TYPE:
        return WaitObjectType::SmbIo;
    default:
        break;
    }
    return WaitObjectType::Unknown;
}

/// The WCT_OBJECT_STATUS @p raw as Platform's status; Unknown for WctStatusUnknown and any value this
/// build doesn't know.
[[nodiscard]] constexpr WaitNodeStatus nodeStatus(std::uint32_t raw) noexcept
{
    switch (raw)
    {
    case WCT_STATUS_NO_ACCESS:
        return WaitNodeStatus::NoAccess;
    case WCT_STATUS_RUNNING:
        return WaitNodeStatus::Running;
    case WCT_STATUS_BLOCKED:
        return WaitNodeStatus::Blocked;
    case WCT_STATUS_PID_ONLY:
        return WaitNodeStatus::PidOnly;
    case WCT_STATUS_PID_ONLY_RPCSS:
        return WaitNodeStatus::PidOnlyRpcss;
    case WCT_STATUS_OWNED:
        return WaitNodeStatus::Owned;
    case WCT_STATUS_NOT_OWNED:
        return WaitNodeStatus::NotOwned;
    case WCT_STATUS_ABANDONED:
        return WaitNodeStatus::Abandoned;
    case WCT_STATUS_ERROR:
        return WaitNodeStatus::Error;
    default:
        break;
    }
    return WaitNodeStatus::Unknown;
}

/// One WAITCHAIN_NODE_INFO as WindowsWaitChain.cpp copies it out: the type and status as Windows gave
/// them, and the union's member that type uses. A thread node (WctThreadType) carries ThreadObject's
/// fields (only ProcessId when its status is PidOnly: an ALPC or COM server whose thread isn't known);
/// any other node LockObject's name, converted to UTF-8.
struct RawWaitNode
{
    std::string objectName; ///< LockObject.ObjectName, UTF-8: an object node's
    std::uint32_t objectType = 0;
    std::uint32_t objectStatus = 0;
    std::uint32_t processId = 0; ///< ThreadObject.ProcessId: a thread node's
    std::uint32_t threadId = 0;  ///< ThreadObject.ThreadId: a thread node's; 0 for PidOnly
    std::uint32_t waitTime = 0;  ///< ThreadObject.WaitTime
    std::uint32_t contextSwitches = 0;
};

/// A PID or TID as Platform's signed value; one past INT32_MAX (never a real Windows ID) reads as 0, unknown.
[[nodiscard]] constexpr std::int32_t toId(std::uint32_t id) noexcept
{
    return id <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ? static_cast<std::int32_t>(id) : 0;
}

/// @p raw as Platform's node. Only the fields its type uses are carried over: an object's name, or a
/// thread's IDs, wait time and context switches.
[[nodiscard]] inline WaitChainNode decodeNode(const RawWaitNode& raw)
{
    WaitChainNode node;
    node.type = objectType(raw.objectType);
    node.status = nodeStatus(raw.objectStatus);
    node.rawType = raw.objectType;
    if (raw.objectType == WCT_THREAD_TYPE)
    {
        node.pid = toId(raw.processId);
        node.tid = toId(raw.threadId);
        node.waitTime = raw.waitTime;
        node.contextSwitches = raw.contextSwitches;
    }
    else
    {
        node.objectName = raw.objectName;
    }
    return node;
}

/// Thread @p tid's chain from GetThreadWaitChain's nodes: @p nodes as it returned them (at most
/// MAX_NODE_COUNT are kept), @p isCycle its IsCycle, and @p truncated whether it said the chain was
/// longer (ERROR_MORE_DATA). A node Windows never filled in (type 0, left as the zeroed array had it)
/// ends the chain there.
[[nodiscard]] inline ThreadWaitChain decodeChain(std::int32_t tid, std::span<const RawWaitNode> nodes, bool isCycle, bool truncated)
{
    ThreadWaitChain chain;
    chain.tid = tid;
    chain.isCycle = isCycle;
    chain.truncated = truncated || nodes.size() > MAX_NODE_COUNT;
    const std::size_t count = std::min<std::size_t>(nodes.size(), MAX_NODE_COUNT);
    chain.nodes.reserve(count);
    for (const RawWaitNode& raw : nodes.first(count))
    {
        if (raw.objectType == 0)
        {
            break;
        }
        chain.nodes.push_back(decodeNode(raw));
    }
    return chain;
}

/// What a read that tried @p readCount threads reports when @p deniedCount of them were refused for want
/// of access: PermissionDenied when every one was, Ok otherwise, so the chains that were read, and why
/// the others weren't, are shown. A process with no thread tried at all (none listed) is Ok with nothing
/// to show.
[[nodiscard]] constexpr WaitChainReadStatus readStatus(std::size_t readCount, std::size_t deniedCount) noexcept
{
    return readCount > 0 && deniedCount == readCount ? WaitChainReadStatus::PermissionDenied : WaitChainReadStatus::Ok;
}

/// The phrase a thread whose chain couldn't be read shows for Win32 error @p error: Windows' message
/// @p systemMessage for anything but the errors a user meets, which get plain words.
[[nodiscard]] inline std::string chainErrorText(std::uint32_t error, std::string_view systemMessage)
{
    constexpr std::uint32_t ERROR_ACCESS_DENIED_VALUE = 5;
    constexpr std::uint32_t ERROR_INVALID_PARAMETER_VALUE = 87;
    constexpr std::uint32_t ERROR_OBJECT_NOT_FOUND_VALUE = 4312;
    switch (error)
    {
    case ERROR_ACCESS_DENIED_VALUE:
        return "Access denied";
    case ERROR_INVALID_PARAMETER_VALUE:
    case ERROR_OBJECT_NOT_FOUND_VALUE:
        return "The thread has exited";
    default:
        break;
    }
    std::string text(systemMessage);
    // FormatMessage ends its message with ".\r\n"; the dialog shows it mid-line.
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.'))
    {
        text.pop_back();
    }
    if (text.empty())
    {
        return "Error " + std::to_string(error);
    }
    return text;
}

/// Whether a read must stop before its next thread: @p threadsRead of them read already, against
/// WAIT_CHAIN_MAX_THREADS, or @p elapsedMs past the time budget.
[[nodiscard]] constexpr bool budgetSpent(std::size_t threadsRead, std::uint64_t elapsedMs) noexcept
{
    return threadsRead >= WAIT_CHAIN_MAX_THREADS || elapsedMs >= WAIT_CHAIN_TIME_BUDGET_MS;
}

} // namespace Platform::WaitChainMath
