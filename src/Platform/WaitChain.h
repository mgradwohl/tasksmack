#pragma once

// Analyze wait chain (#3084): what each thread of a process is waiting for, as Task Manager and Resource
// Monitor show it for a process that has stopped responding. Windows reads it with the Wait Chain
// Traversal API (GetThreadWaitChain, wct.h): per thread, a chain that starts with the thread itself and
// goes on through the object it waits on (a critical section, a mutex, a SendMessage, an ALPC or COM
// call, another thread or process) to the thread that owns that object, and on, until a thread that
// isn't waiting, a node it can't follow, or a cycle: a deadlock.
//
// Pure data: IProcessActions::readWaitChains() returns it (on a worker, never on the UI thread), and
// App/Panels/ProcessWaitChainDialog.h shows it. Linux has no such API, so it never reads one
// (ProcessActionCapabilities::canAnalyzeWaitChain is false there).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Platform
{

/// What one node of a wait chain is: the thread type, or the kind of object a thread waits on. Mirrors
/// WCT_OBJECT_TYPE; Unknown is any value this build doesn't know (WaitChainNode::rawType keeps it).
enum class WaitObjectType : std::uint8_t
{
    CriticalSection, ///< A critical section (EnterCriticalSection)
    SendMessageCall, ///< A SendMessage call to another thread's window, waiting for its answer
    Mutex,           ///< A mutex (mutant)
    Alpc,            ///< An ALPC call (a local RPC) to another process
    Com,             ///< A COM call to another apartment or process
    ThreadWait,      ///< A wait on a thread's handle: for it to exit
    ProcessWait,     ///< A wait on a process's handle: for it to exit
    Thread,          ///< A thread: the chain's first node, and each thread that owns what the one before waits on
    ComActivation,   ///< A COM activation (CoCreateInstance) waiting on the server that creates the object
    SocketIo,        ///< A socket's I/O
    SmbIo,           ///< An SMB (network file share) I/O
    Unknown,         ///< Anything else: Windows' own "unknown", or a type newer than this build
};

/// A node's state. Mirrors WCT_OBJECT_STATUS; Unknown is also any value this build doesn't know.
enum class WaitNodeStatus : std::uint8_t
{
    Unknown,
    NoAccess,     ///< The node's details couldn't be read: another user's or a protected process
    Running,      ///< A thread that is running (or ready): not waiting
    Blocked,      ///< A thread that is waiting
    PidOnly,      ///< Only the owning process is known (an ALPC or COM server), not its thread
    PidOnlyRpcss, ///< Only the owning process is known, and it is RPCSS (the COM activation service)
    Owned,        ///< An object some thread owns
    NotOwned,     ///< An object no thread owns
    Abandoned,    ///< A mutex whose owner exited without releasing it
    Error,        ///< Windows couldn't read the node
};

/// One node of a thread's wait chain: a thread (type Thread) or what a thread waits on.
struct WaitChainNode
{
    // An object node's name (a named mutex's, a file's), UTF-8; empty when it has none. A thread node has none.
    std::string objectName;
    // The name of process `pid` (its executable's file name, "notepad.exe"), when it could be found.
    std::string processName;
    // Process `pid`'s start time (ProcessTarget::startTimeTicks), so End process names exactly that process;
    // 0 when it couldn't be read (another user's process): End process is then not offered.
    std::uint64_t processStartTimeTicks = 0;
    std::uint32_t rawType = 0; ///< The OS's own value for the type, so an Unknown one can still be named
    // A thread node's, and a PidOnly node's, process: 0 when unknown.
    std::int32_t pid = 0;
    std::int32_t tid = 0;              ///< A thread node's thread; 0 for an object
    std::uint32_t waitTime = 0;        ///< A thread node: its wait time, as Windows reports it (ThreadObject.WaitTime)
    std::uint32_t contextSwitches = 0; ///< A thread node: how many times it has been switched in
    WaitObjectType type = WaitObjectType::Unknown;
    WaitNodeStatus status = WaitNodeStatus::Unknown;
};

/// One thread's chain: the thread itself first, then what it waits on, and so on.
struct ThreadWaitChain
{
    std::vector<WaitChainNode> nodes;
    // Why the chain couldn't be read (the thread exited meanwhile, access denied), as a phrase the dialog
    // shows; empty when it was read. `nodes` is then empty.
    std::string error;
    std::int32_t tid = 0;
    bool isCycle = false;   ///< The chain loops back on itself: these threads are deadlocked
    bool truncated = false; ///< The chain is longer than WAIT_CHAIN_MAX_NODES; `nodes` holds its start
};

/// What came of one wait chain read.
enum class WaitChainReadStatus : std::uint8_t
{
    Ok,               ///< Read; `threads` holds every thread's chain read (or why one couldn't be)
    PermissionDenied, ///< Windows refused: no thread's chain could be read for want of access
    ProcessExited,    ///< The process is gone, or its PID now belongs to a different process
    Unsupported,      ///< This platform or run can't read wait chains (Linux, synthetic runs)
    IdentityUnknown,  ///< The target's start time is unknown (0), so the process couldn't be confirmed; nothing was read
    Cancelled,        ///< The read was asked to stop (the dialog closed) before it finished
    Failed,           ///< Any other error; `detail` says what
};

/// The outcome of IProcessActions::readWaitChains().
struct WaitChainReadResult
{
    std::vector<ThreadWaitChain> threads; ///< In the order the threads were listed
    std::string detail;                   ///< For Failed, what failed; for Unsupported, why; else empty
    std::size_t threadCount = 0;          ///< The threads the process had when listed: more than `threads` holds when cut short
    WaitChainReadStatus status = WaitChainReadStatus::Unsupported;
    // Not every thread was read: the process had more than WAIT_CHAIN_MAX_THREADS, or the read ran past
    // WAIT_CHAIN_TIME_BUDGET_MS.
    bool threadsTruncated = false;
};

/// The longest chain a read follows: Windows' own limit (WCT_MAX_NODE_COUNT).
inline constexpr std::size_t WAIT_CHAIN_MAX_NODES = 16;
/// The most threads one read follows; a process with more is cut short (WaitChainReadResult::threadsTruncated).
inline constexpr std::size_t WAIT_CHAIN_MAX_THREADS = 1024;
/// How long one read may take, in ms: each GetThreadWaitChain call can ask another process (an ALPC or
/// COM server) and wait for it, so the threads after this are left out rather than the dialog waiting on.
inline constexpr std::uint32_t WAIT_CHAIN_TIME_BUDGET_MS = 5000;

/// What readWaitChains() says where the platform can't read wait chains.
inline constexpr const char* WAIT_CHAIN_UNSUPPORTED_MESSAGE = "Analyzing a wait chain isn't available on this system";

} // namespace Platform
