#pragma once

// The privileged-broker protocol (#2306; #2181, decision 4): what TaskSmack, never elevated itself, asks Windows'
// elevated helper (TaskSmackHelper.exe) over a private named pipe (#2311). Linux had a helper too, until #3580 (#3603)
// removed it: it never creates a broker now, so the messages only it served or only Linux sent are gone (v23). Pure
// encode and decode, no I/O, so it is unit-tested and fuzzed on every platform (tests/Platform/test_BrokerProtocol.cpp,
// tests/fuzz/fuzz_broker_protocol.cpp).
//
// A message is one frame, little-endian:
//
//   u32 length    bytes after this field (version + kind + request id + payload)
//   u16 version   PROTOCOL_VERSION; any other is refused, never guessed at
//   u16 kind      MessageKind
//   u32 requestId the request a response answers (the client numbers its requests)
//   payload       the kind's fields, in order; strings and lists carry a u32 count first
//
// Decoding is strict, since the helper reads what an unprivileged process sends: a frame of exactly its
// length, a known kind, every enum in range, every count within its cap, nothing left over. Anything
// else is refused as malformed. There is no "run a command", and every process request names its target
// by PID *and* start time (ProcessTarget), so the helper re-verifies the process and a reused PID can't
// redirect a request. The one path a request carries is Replace Task Manager's (#3072), which the helper
// takes only when it is the TaskSmack.exe beside it. Its kinds: the handshake, the process actions
// (#2308), Process Details' reads (#2309), Replace Task Manager (#3072), the Users tab's Disconnect and
// Sign out of another user's Windows session (#3076), Find handles' batch of open-files reads (#3578), and
// Process Details' memory map (#3665), a read of the region list only, never of the process's memory.

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessMemoryMap.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessThreads.h"
#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"
#include "Platform/IoPriorityTypes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Platform::Broker
{

/// Bumped on every change to a message's fields: the helper and TaskSmack ship together, and a Hello of
/// another version is refused rather than misread. 2: ModulesResult and EnvironmentResult say whether
/// their list was cut to fit a frame (#2403). 3: Hello says whether the session is held (#2728). 4: ReadProcessCounters (#2728).
/// 5: ReadProcessCounters can also ask for each process's socket inodes, for network attribution (#2728).
/// 6: ListHiddenProcesses, the /proc/<pid> files of processes hidepid hides from TaskSmack (#2728).
/// 7: ServiceAction, a systemd unit started, stopped or given a start type over the helper's bus (#2728).
/// 8: Signal gains Hangup, Interrupt, Quit, User1 and User2 (#3063).
/// 9: SetEfficiencyMode, Efficiency mode turned on or off for a process (#3071).
/// 10: CloseConnection, one of a process's sockets shut down (#3066).
/// 11: ReadThreads, another user's process's threads for Process Details (#3069).
/// 12: TaskManagerReplacement, TaskSmack set or cleared as Task Manager's IFEO debugger (#3072).
/// 13: SessionAction, another user's Windows session disconnected or signed out (#3076).
/// 14: ThreadAction, one thread suspended, resumed or ended (#3069, Windows); ThreadsResult carries each
///     thread's wait reason, suspend count and creation time, and whether its priority is Windows' rather than a nice value.
/// 15: SetSchedPolicy, a process's CPU scheduling policy set to Normal, Batch or Idle (#3086, Linux).
/// 16: ReadMemoryMap, another user's process's memory map for Process Details (#3089).
/// 17: SetSchedPolicy carries a real-time priority, and may set FIFO or round robin with 1 to 99 (#3219, Linux).
/// 18: SetIoPriority's class can be WindowsHint, a Windows I/O priority hint (#3362, Windows).
/// 19: ReadUdpBytes, every process's UDP bytes from the Linux helper's eBPF counter (#3366, Linux).
/// 20: CloseConnection carries an IPv4 TCP connection's endpoints, which name it on Windows (#3358, Windows).
/// 21: HiddenProcessesResult carries each hidden process's raw I/O priority, read by the helper (#3572, Linux).
/// 22: SetEfficiencyMode carries the priority class an off restores, and ActionResult the class an on replaced, so
///     TaskSmack remembers it wherever the on ran (#3327, Windows).
/// 23: the messages only the Linux helper served or only Linux sent are gone: SetSchedPolicy, ServiceAction,
///     ReadMemoryRollup, ReadFirmwareFile, ReadCrashes, ReadProcessCounters, ListHiddenProcesses, ReadMemoryMap,
///     ReadUdpBytes and their results, and Hello's forSession (#3580). The kinds left keep their numbers.
/// 24: ReadOpenFilesBatch, many processes' open files from one walk of Windows' handle table, for Find handles'
///     administrator pass (#3578), answered with OpenFilesBatchResult. Its kinds take numbers never used before.
/// 25: ReadMemoryMap again, now that Windows' helper serves it: an elevated or another user's process's memory map
///     for Process Details (#3665), answered with MemoryMapResult. The fields are v16's; the kinds take numbers
///     never used before (45 and 64), as v24's did.
inline constexpr std::uint16_t PROTOCOL_VERSION = 25;
/// The largest frame either side accepts, header included.
inline constexpr std::size_t MAX_MESSAGE_BYTES = std::size_t{8} << 20U;
/// The longest string field (an error message, a note).
inline constexpr std::size_t MAX_STRING_BYTES = std::size_t{64} << 10U;
/// The most environment variables an EnvironmentReply carries; a read with more keeps the first. Fewer
/// fit when they are long: every list reply stops short of MAX_MESSAGE_BYTES and says it was cut (#2403).
inline constexpr std::size_t MAX_ENVIRONMENT_VARIABLES = std::size_t{1} << 16U;
/// The most modules a ModulesReply carries; a read with more keeps the first.
inline constexpr std::size_t MAX_MODULES = std::size_t{1} << 16U;
/// The most sockets a ConnectionsReply carries; a read with more keeps the first.
inline constexpr std::size_t MAX_CONNECTIONS = std::size_t{1} << 16U;
/// The most threads a ThreadsReply carries (#3069); a read with more keeps the first.
inline constexpr std::size_t MAX_THREADS = std::size_t{1} << 16U;
/// The most regions a MemoryMapReply carries (#3665): the Windows reader's own cap (Windows::MAX_MEMORY_MAP_REGIONS;
/// HelperMain.cpp static_asserts it fits), so a read the helper kept whole crosses whole when the frame holds it. A
/// frame holds fewer (at least 31 bytes each) when their paths are long, and says it was cut.
inline constexpr std::size_t MAX_MEMORY_MAPPINGS = std::size_t{1} << 16U;
/// The most processes one ReadOpenFilesBatch names (#3578); a search with more sends several.
inline constexpr std::size_t MAX_OPEN_FILES_BATCH = 1024;
/// The frame header after the length: version, kind, request id.
inline constexpr std::size_t HEADER_BYTES = 2 + 2 + 4;

enum class MessageKind : std::uint16_t // NOLINT(performance-enum-size): 16 bits on the wire
{
    Hello = 1,      ///< Client -> helper, first: the client's version
    HelloReply = 2, ///< Helper -> client: whether it speaks that version
    Signal = 16,    ///< Terminate, kill, suspend or resume a process
    SetPriority = 17,
    SetIoPriority = 18,
    SetAffinity = 19,
    SetEfficiencyMode = 21,      ///< Efficiency mode on or off for a process (#3071)
    CloseConnection = 22,        ///< One of a process's sockets shut down, by inode (#3066)
    TaskManagerReplacement = 23, ///< TaskSmack set or cleared as Task Manager's IFEO debugger (#3072)
    SessionAction = 24,          ///< Another user's Windows session disconnected or signed out (#3076)
    ThreadAction = 25,           ///< One thread of a process suspended, resumed or ended (#3069, Windows)
    ReadEnvironment = 32,        ///< Another user's process's environment, for Process Details (#2309)
    ReadModules = 33,            ///< Its loaded modules (#2309)
    ReadOpenFiles = 34,          ///< Its open files (#2309)
    ReadConnections = 35,        ///< Its TCP and UDP sockets (#2309)
    ReadThreads = 41,            ///< Another user's process's threads, for Process Details (#3069)
    ReadOpenFilesBatch = 44,     ///< Many processes' open files from one handle-table walk, for Find handles (#3578)
    ReadMemoryMap = 45,          ///< An elevated or another user's process's memory map, for Process Details (#3665)
    ActionResult = 48,           ///< The answer to a process action
    EnvironmentResult = 49,      ///< The answer to ReadEnvironment
    ModulesResult = 50,          ///< The answer to ReadModules
    OpenFilesResult = 51,        ///< The answer to ReadOpenFiles
    ConnectionsResult = 52,      ///< The answer to ReadConnections
    ServiceActionResult = 58,    ///< The answer to SessionAction (#3076)
    ThreadsResult = 59,          ///< The answer to ReadThreads
    OpenFilesBatchResult = 62,   ///< The answer to ReadOpenFilesBatch (#3578)
    Error = 63,                  ///< A request the helper refused to read (malformed, or a kind it doesn't do)
    MemoryMapResult = 64,        ///< The answer to ReadMemoryMap (#3665)
};

/// The signals a Signal request sends.
enum class Signal : std::uint8_t
{
    Terminate,
    Kill,
    Suspend,
    Resume,
    Hangup, ///< protocol v8 (#3063): the signals ProcessSignal names
    Interrupt,
    Quit,
    User1,
    User2,
};

struct Hello
{
    std::uint16_t version = PROTOCOL_VERSION;
    friend bool operator==(const Hello&, const Hello&) = default;
};

struct HelloReply
{
    std::uint16_t version = PROTOCOL_VERSION;
    bool accepted = false;
    friend bool operator==(const HelloReply&, const HelloReply&) = default;
};

struct SignalRequest
{
    ProcessTarget target;
    Signal signal = Signal::Terminate;
    friend bool operator==(const SignalRequest&, const SignalRequest&) = default;
};

struct PriorityRequest
{
    ProcessTarget target;
    std::int32_t nice = 0;
    friend bool operator==(const PriorityRequest&, const PriorityRequest&) = default;
};

struct IoPriorityRequest
{
    ProcessTarget target;
    IoPriorityClass ioClass = IoPriorityClass::BestEffort;
    std::int32_t level = 4;
    friend bool operator==(const IoPriorityRequest&, const IoPriorityRequest&) = default;
};

struct AffinityRequest
{
    ProcessTarget target;
    CpuAffinity cpus;
    friend bool operator==(const AffinityRequest&, const AffinityRequest&) = default;
};

/// Turn Efficiency mode on or off for a process (#3071, protocol v9): IProcessActions::setEfficiencyMode(). An off
/// carries the priority class TaskSmack remembers the on replaced (#3327, protocol v22; Windows): the helper restores
/// it, though the on ran in TaskSmack or in an earlier helper. 0: TaskSmack doesn't know it, and the helper's off
/// restores what it remembers itself, or Normal, saying so.
struct EfficiencyModeRequest
{
    ProcessTarget target;
    bool on = false;
    std::uint32_t restorePriorityClass = 0; ///< An off: the Windows priority class to restore; 0 unknown
    friend bool operator==(const EfficiencyModeRequest&, const EfficiencyModeRequest&) = default;
};

/// Shut down one of a process's sockets (#3066, protocol v10): IProcessActions::closeConnection(). The inode names
/// the socket; the helper finds it among the target's descriptors after the identity check. On Windows (#3358,
/// protocol v20) the inode is 0 and the IPv4 TCP endpoints name it instead: IProcessActions::closeTcpConnection().
struct CloseConnectionRequest
{
    ProcessTarget target;
    std::uint64_t socketInode = 0;
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    TcpConnectionToClose tcpConnection{};
    friend bool operator==(const CloseConnectionRequest&, const CloseConnectionRequest&) = default;
};

/// What a TaskManagerReplacementRequest does (#3072).
enum class TaskManagerChange : std::uint8_t
{
    Set,   ///< Task Manager starts the TaskSmack.exe the request names
    Clear, ///< Task Manager starts itself again: the Debugger TaskSmack set is removed
};

/// Replace Task Manager (#3072, protocol v12): the Image File Execution Options Debugger of taskmgr.exe set
/// to @c path, or cleared. The helper sets only a @c path that is the TaskSmack.exe beside it (validated
/// with TaskManagerReplacement::validateTaskSmackPath() and BrokerHelper::sameFile()), and replaces another
/// program's Debugger only with @c replaceOther, which TaskSmack sends once the user has confirmed it. A
/// Clear carries no path and never replaceOther; one that does is refused as malformed. The answer is an
/// ActionResult.
struct TaskManagerReplacementRequest
{
    TaskManagerChange change = TaskManagerChange::Set;
    std::string path; ///< UTF-8; empty for a Clear
    bool replaceOther = false;
    friend bool operator==(const TaskManagerReplacementRequest&, const TaskManagerReplacementRequest&) = default;
};

// ---- The Users tab's session actions (#3076, protocol v13) ----

/// Disconnect or sign out another user's Windows session (#3076, protocol v13): what TaskSmack can't do
/// without an administrator's rights. Only Disconnect and SignOut are carried (a Lock is refused as
/// malformed). The session is named by its WTS ID and by the sign-in the user confirmed: its user, domain
/// and sign-in time, since Windows reuses session IDs. The helper checks it against a fresh list before it
/// acts (UserSessionsMath::checkSessionAction(): not session 0, still there, the same sign-in), and refuses
/// a request with no user or sign-in time. The answer is a ServiceActionReply.
struct SessionActionRequest
{
    SessionAction action = SessionAction::SignOut;
    std::uint32_t sessionId = 0;
    std::string user;            ///< The confirmed session's user (UTF-8)
    std::string domain;          ///< Its domain or computer name
    std::uint64_t logonUsec = 0; ///< When its user signed in (CLOCK_REALTIME µs)
    friend bool operator==(const SessionActionRequest&, const SessionActionRequest&) = default;
};

// ---- end of the session actions ----

/// Suspend, resume or end one thread of a process (#3069, protocol v14): IProcessActions::actOnThread(). The
/// helper checks the process's identity, that the thread belongs to it, and the same guards as in TaskSmack
/// (never the System process's, a critical process's, its own or TaskSmack's threads). The answer is an
/// ActionResult. The thread's creation time is checked too, so a TID reused since the user confirmed is refused.
struct ThreadActionRequest
{
    ProcessTarget target;
    std::int32_t tid = 0;
    std::uint64_t threadCreateTime = 0; ///< The confirmed thread's creation time; the helper refuses another's
    ThreadAction action = ThreadAction::Suspend;
    friend bool operator==(const ThreadActionRequest&, const ThreadActionRequest&) = default;
};

/// Read a process's environment (#2309): the helper reads it with its capabilities, after the same identity check.
struct EnvironmentRequest
{
    ProcessTarget target;
    friend bool operator==(const EnvironmentRequest&, const EnvironmentRequest&) = default;
};

/// The answer to EnvironmentRequest: the read's result, as IProcessEnvironmentReader returns it.
struct EnvironmentReply
{
    EnvironmentReadResult result;

    friend bool operator==(const EnvironmentReply& left, const EnvironmentReply& right)
    {
        if (left.result.status != right.result.status || left.result.truncated != right.result.truncated ||
            left.result.variables.size() != right.result.variables.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < left.result.variables.size(); ++i)
        {
            if (left.result.variables[i].name != right.result.variables[i].name ||
                left.result.variables[i].value != right.result.variables[i].value)
            {
                return false;
            }
        }
        return true;
    }
};

/// List a process's loaded modules (#2309), as the helper reads them with its capabilities.
struct ModulesRequest
{
    ProcessTarget target;
    friend bool operator==(const ModulesRequest&, const ModulesRequest&) = default;
};

/// The answer to ModulesRequest: the read's result, as IProcessModulesReader returns it.
struct ModulesReply
{
    ModulesReadResult result;

    friend bool operator==(const ModulesReply& left, const ModulesReply& right)
    {
        const auto same = [](const ProcessModule& a, const ProcessModule& b)
        {
            return a.path == b.path && a.baseAddress == b.baseAddress && a.sizeBytes == b.sizeBytes && a.version == b.version &&
                   a.deleted == b.deleted;
        };
        return left.result.status == right.result.status && left.result.detail == right.result.detail &&
               left.result.truncated == right.result.truncated && std::ranges::equal(left.result.modules, right.result.modules, same);
    }
};

/// List a process's open files (#2309), as the helper reads them with its capabilities.
struct OpenFilesRequest
{
    ProcessTarget target;
    friend bool operator==(const OpenFilesRequest&, const OpenFilesRequest&) = default;
};

/// Whether two open-files reads say the same thing, field by field (OpenFilesReadResult has no operator==).
[[nodiscard]] inline bool sameOpenFilesResult(const OpenFilesReadResult& left, const OpenFilesReadResult& right)
{
    const auto same = [](const OpenFile& a, const OpenFile& b)
    {
        return a.descriptor == b.descriptor && a.kind == b.kind && a.path == b.path && a.flags == b.flags && a.deleted == b.deleted;
    };
    return left.status == right.status && left.detail == right.detail && left.truncated == right.truncated &&
           left.namesIncomplete == right.namesIncomplete && left.hexDescriptors == right.hexDescriptors &&
           std::ranges::equal(left.files, right.files, same);
}

/// The answer to OpenFilesRequest: the read's result, as IProcessOpenFilesReader returns it.
struct OpenFilesReply
{
    OpenFilesReadResult result;

    friend bool operator==(const OpenFilesReply& left, const OpenFilesReply& right)
    {
        return sameOpenFilesResult(left.result, right.result);
    }
};

/// List many processes' open files at once (#3578, protocol v24): Find handles' administrator pass, which the helper
/// answers from one walk of the system handle table (IProcessOpenFilesReader::beginBatch()) rather than one per
/// process. One to MAX_OPEN_FILES_BATCH targets; the reader checks each one's identity, as for OpenFilesRequest.
struct OpenFilesBatchRequest
{
    std::vector<ProcessTarget> targets;
    friend bool operator==(const OpenFilesBatchRequest&, const OpenFilesBatchRequest&) = default;
};

/// The answer to OpenFilesBatchRequest: the first targets' results, in the request's order. Fewer than it named when
/// the rest didn't fit one frame or the helper's time for one request (BrokerServer::OPEN_FILES_BATCH_BUDGET): the
/// client asks again for those. At least one when the request named any, the first cut to fit as an OpenFilesReply is.
struct OpenFilesBatchReply
{
    std::vector<OpenFilesReadResult> results;

    friend bool operator==(const OpenFilesBatchReply& left, const OpenFilesBatchReply& right)
    {
        return std::ranges::equal(left.results, right.results, sameOpenFilesResult);
    }
};

/// List a process's TCP and UDP sockets (#2309), as the helper reads them with its capabilities.
struct ConnectionsRequest
{
    ProcessTarget target;
    friend bool operator==(const ConnectionsRequest&, const ConnectionsRequest&) = default;
};

/// List a process's threads (#3069), as the helper reads them with its capabilities.
struct ThreadsRequest
{
    ProcessTarget target;
    friend bool operator==(const ThreadsRequest&, const ThreadsRequest&) = default;
};

/// The answer to ThreadsRequest: the read's result, as IProcessThreadsReader returns it.
struct ThreadsReply
{
    ThreadsReadResult result;

    friend bool operator==(const ThreadsReply& left, const ThreadsReply& right)
    {
        const auto same = [](const ProcessThread& a, const ProcessThread& b)
        {
            return a.tid == b.tid && a.name == b.name && a.state == b.state && a.userTicks == b.userTicks &&
                   a.systemTicks == b.systemTicks && a.nice == b.nice && a.startTicks == b.startTicks && a.waitReason == b.waitReason &&
                   a.suspendCount == b.suspendCount && a.createTime == b.createTime;
        };
        return left.result.status == right.result.status && left.result.detail == right.result.detail &&
               left.result.ticksPerSecond == right.result.ticksPerSecond && left.result.truncated == right.result.truncated &&
               left.result.bootTimeEpoch == right.result.bootTimeEpoch && left.result.windowsPriorities == right.result.windowsPriorities &&
               std::ranges::equal(left.result.threads, right.result.threads, same);
    }
};

/// Read a process's memory map (#3665), as the helper reads it with its rights. It names only the target: the
/// helper walks that process's address space, never anything the request could choose, and reads nothing out of
/// its memory.
struct MemoryMapRequest
{
    ProcessTarget target;
    friend bool operator==(const MemoryMapRequest&, const MemoryMapRequest&) = default;
};

/// The answer to MemoryMapRequest: the read's result, as IProcessMemoryMapReader returns it.
struct MemoryMapReply
{
    MemoryMapReadResult result;

    friend bool operator==(const MemoryMapReply& left, const MemoryMapReply& right)
    {
        return left.result.status == right.result.status && left.result.detail == right.result.detail &&
               left.result.truncated == right.result.truncated && left.result.mappings == right.result.mappings;
    }
};

/// The answer to ConnectionsRequest: the read's result, as IProcessConnectionsReader returns it.
struct ConnectionsReply
{
    ConnectionsReadResult result;

    friend bool operator==(const ConnectionsReply& left, const ConnectionsReply& right)
    {
        const auto sameEnd = [](const ConnectionEndpoint& a, const ConnectionEndpoint& b)
        {
            return a.address == b.address && a.port == b.port;
        };
        const auto same = [&sameEnd](const ProcessConnection& a, const ProcessConnection& b)
        {
            return a.protocol == b.protocol && a.family == b.family && sameEnd(a.local, b.local) && sameEnd(a.remote, b.remote) &&
                   a.state == b.state;
        };
        return left.result.status == right.result.status && left.result.detail == right.result.detail &&
               std::ranges::equal(left.result.connections, right.result.connections, same);
    }
};

/// The answer to SessionActionRequest (#3076): the action's result, as TaskSmack's own call would have given it.
struct ServiceActionReply
{
    ServiceActionResult result;
    friend bool operator==(const ServiceActionReply& left, const ServiceActionReply& right)
    {
        return left.result.ok == right.result.ok && left.result.message == right.result.message &&
               left.result.cancelled == right.result.cancelled && left.result.needsAuthorization == right.result.needsAuthorization &&
               left.result.authorizationUnavailable == right.result.authorizationUnavailable;
    }
};

/// The answer to a process action: ProcessActionResult's fields.
struct ActionResult
{
    bool success = false;
    std::string errorMessage;
    std::string successNote;
    /// An Efficiency mode on's: the priority class it replaced, for TaskSmack to remember (#3327, protocol v22); else 0.
    std::uint32_t priorityClassBefore = 0;
    friend bool operator==(const ActionResult&, const ActionResult&) = default;

    [[nodiscard]] static ActionResult from(const ProcessActionResult& result)
    {
        return {
            .success = result.success,
            .errorMessage = result.errorMessage,
            .successNote = result.successNote,
            .priorityClassBefore = result.priorityClassBefore,
        };
    }
    [[nodiscard]] ProcessActionResult toProcessActionResult() const
    {
        ProcessActionResult result = success ? ProcessActionResult::ok(successNote) : ProcessActionResult::error(errorMessage);
        result.priorityClassBefore = priorityClassBefore;
        return result;
    }
};

struct ErrorReply
{
    std::string message;
    friend bool operator==(const ErrorReply&, const ErrorReply&) = default;
};

using Request = std::variant<Hello,
                             SignalRequest,
                             PriorityRequest,
                             IoPriorityRequest,
                             AffinityRequest,
                             EnvironmentRequest,
                             ModulesRequest,
                             OpenFilesRequest,
                             ConnectionsRequest,
                             EfficiencyModeRequest,
                             CloseConnectionRequest,
                             ThreadsRequest,
                             TaskManagerReplacementRequest,
                             SessionActionRequest,
                             ThreadActionRequest,
                             OpenFilesBatchRequest,
                             MemoryMapRequest>;
using Response = std::variant<HelloReply,
                              ActionResult,
                              ErrorReply,
                              EnvironmentReply,
                              ModulesReply,
                              OpenFilesReply,
                              ConnectionsReply,
                              ServiceActionReply,
                              ThreadsReply,
                              OpenFilesBatchReply,
                              MemoryMapReply>;

/// A decoded frame: its request id and message.
template<typename Message> struct Decoded
{
    std::uint32_t requestId = 0;
    Message message;
};

namespace Detail
{

class Writer
{
  public:
    void u8(std::uint8_t value)
    {
        m_Bytes.push_back(value);
    }
    void u16(std::uint16_t value)
    {
        for (int shift = 0; shift < 16; shift += 8)
        {
            m_Bytes.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
        }
    }
    void u32(std::uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            m_Bytes.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
        }
    }
    void u64(std::uint64_t value)
    {
        for (int shift = 0; shift < 64; shift += 8)
        {
            m_Bytes.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
        }
    }
    void i32(std::int32_t value)
    {
        u32(static_cast<std::uint32_t>(value));
    }
    void text(std::string_view value)
    {
        const std::string_view capped = value.substr(0, MAX_STRING_BYTES); // never more than the reader takes
        u32(static_cast<std::uint32_t>(capped.size()));
        m_Bytes.insert(m_Bytes.end(), capped.begin(), capped.end());
    }
    void target(const ProcessTarget& value)
    {
        i32(value.pid);
        u64(value.startTimeTicks);
    }

    /// How many payload bytes have been written so far.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Bytes.size();
    }

    /// The frame: length, version, kind, request id, then what was written.
    [[nodiscard]] std::vector<std::uint8_t> frame(MessageKind kind, std::uint32_t requestId) const
    {
        Writer header;
        header.u32(static_cast<std::uint32_t>(HEADER_BYTES + m_Bytes.size()));
        header.u16(PROTOCOL_VERSION);
        header.u16(static_cast<std::uint16_t>(kind));
        header.u32(requestId);
        std::vector<std::uint8_t> out = std::move(header.m_Bytes);
        out.insert(out.end(), m_Bytes.begin(), m_Bytes.end());
        return out;
    }

  private:
    std::vector<std::uint8_t> m_Bytes;
};

/// The bytes a reply's list may take: a frame's whole, less its length, header and the reply's fixed
/// fields (@p fixedBytes, at their largest).
[[nodiscard]] constexpr std::size_t listRoom(std::size_t fixedBytes) noexcept
{
    return MAX_MESSAGE_BYTES - (4 + HEADER_BYTES) - fixedBytes;
}

/// How many of the first entries of @p entries, at most @p cap, fit in @p room bytes, an entry taking
/// @p bytesOf(entry). A list reply sends that many and says it was cut when it is fewer than all, so a
/// process with tens of thousands of long paths gets a shorter list, not a frame the protocol refuses and
/// a helper that ends the session over it (#2403).
template<typename Entry, typename BytesOf>
[[nodiscard]] std::size_t entriesThatFit(std::span<const Entry> entries, std::size_t cap, std::size_t room, BytesOf bytesOf)
{
    const std::size_t limit = std::min(entries.size(), cap);
    std::size_t count = 0;
    std::size_t used = 0;
    while (count < limit)
    {
        const std::size_t entry = bytesOf(entries[count]);
        if (entry > room - used)
        {
            break;
        }
        used += entry;
        ++count;
    }
    return count;
}

/// The bytes Writer::text() writes for @p value: its length and at most MAX_STRING_BYTES of it.
[[nodiscard]] constexpr std::size_t textBytes(std::string_view value) noexcept
{
    return 4 + std::min(value.size(), MAX_STRING_BYTES);
}

/// Bounds-checked reads; the first one past the end fails the reader, and every later one too.
class Reader
{
  public:
    explicit Reader(std::span<const std::uint8_t> bytes) noexcept : m_Bytes(bytes)
    {}

    [[nodiscard]] bool ok() const noexcept
    {
        return m_Ok;
    }
    [[nodiscard]] bool atEnd() const noexcept
    {
        return m_At == m_Bytes.size();
    }

    std::uint64_t unsignedLittleEndian(std::size_t width) noexcept
    {
        if (!m_Ok || m_Bytes.size() - m_At < width)
        {
            m_Ok = false;
            return 0;
        }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < width; ++i)
        {
            value |= static_cast<std::uint64_t>(m_Bytes[m_At + i]) << (8U * i);
        }
        m_At += width;
        return value;
    }
    std::uint8_t u8() noexcept
    {
        return static_cast<std::uint8_t>(unsignedLittleEndian(1));
    }
    std::uint16_t u16() noexcept
    {
        return static_cast<std::uint16_t>(unsignedLittleEndian(2));
    }
    std::uint32_t u32() noexcept
    {
        return static_cast<std::uint32_t>(unsignedLittleEndian(4));
    }
    std::uint64_t u64() noexcept
    {
        return unsignedLittleEndian(8);
    }
    std::int32_t i32() noexcept
    {
        return static_cast<std::int32_t>(u32());
    }
    bool flag() noexcept
    {
        const std::uint8_t value = u8();
        if (value > 1)
        {
            m_Ok = false;
        }
        return value == 1;
    }
    std::string text()
    {
        const std::uint32_t size = u32();
        if (!m_Ok || size > MAX_STRING_BYTES || m_Bytes.size() - m_At < size)
        {
            m_Ok = false;
            return {};
        }
        std::string value(reinterpret_cast<const char*>(m_Bytes.data() + m_At),
                          size); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast) - bytes as chars
        m_At += size;
        return value;
    }
    ProcessTarget target() noexcept
    {
        const std::int32_t pid = i32();
        const std::uint64_t start = u64();
        return {.pid = pid, .startTimeTicks = start};
    }
    void fail() noexcept
    {
        m_Ok = false;
    }

  private:
    std::span<const std::uint8_t> m_Bytes;
    std::size_t m_At = 0;
    bool m_Ok = true;
};

/// The frame in @p bytes: its kind, request id and payload, or why it isn't one.
struct Frame
{
    MessageKind kind = MessageKind::Error;
    std::uint32_t requestId = 0;
    std::span<const std::uint8_t> payload;
};

[[nodiscard]] inline std::expected<Frame, std::string> readFrame(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 4 + HEADER_BYTES)
    {
        return std::unexpected("A frame shorter than its header");
    }
    if (bytes.size() > MAX_MESSAGE_BYTES)
    {
        return std::unexpected("A frame larger than the protocol allows");
    }
    Reader header(bytes);
    const std::uint32_t length = header.u32();
    if (length != bytes.size() - 4)
    {
        return std::unexpected("A frame whose length doesn't match");
    }
    if (const std::uint16_t version = header.u16(); version != PROTOCOL_VERSION)
    {
        return std::unexpected("A frame of another protocol version");
    }
    const auto kind = static_cast<MessageKind>(header.u16());
    const std::uint32_t requestId = header.u32();
    return Frame{.kind = kind, .requestId = requestId, .payload = bytes.subspan(4 + HEADER_BYTES)};
}

template<typename Message>
[[nodiscard]] std::expected<Decoded<Message>, std::string> finish(const Reader& reader, std::uint32_t id, Message message)
{
    if (!reader.ok() || !reader.atEnd())
    {
        return std::unexpected("A malformed message");
    }
    return Decoded<Message>{.requestId = id, .message = std::move(message)};
}

} // namespace Detail

namespace Detail
{

// One encoder per request kind (#3043), as the decoders below: each writes its request's payload and says
// which kind of message it is. encodeRequest() visits the Request with them, so a kind without an
// encoder doesn't compile.

/// Hello: the client's protocol version.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const Hello& hello)
{
    writer.u16(hello.version);
    return MessageKind::Hello;
}

/// Signal: the target, then the signal.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const SignalRequest& signal)
{
    writer.target(signal.target);
    writer.u8(static_cast<std::uint8_t>(signal.signal));
    return MessageKind::Signal;
}

/// SetPriority: the target, then the nice value.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const PriorityRequest& priority)
{
    writer.target(priority.target);
    writer.i32(priority.nice);
    return MessageKind::SetPriority;
}

/// SetIoPriority: the target, the I/O class, then its level.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const IoPriorityRequest& io)
{
    writer.target(io.target);
    writer.u8(static_cast<std::uint8_t>(io.ioClass));
    writer.i32(io.level);
    return MessageKind::SetIoPriority;
}

/// SetEfficiencyMode (#3071): the target, whether it is turned on (0 or 1), then the class an off restores (#3327).
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const EfficiencyModeRequest& efficiency)
{
    writer.target(efficiency.target);
    writer.u8(efficiency.on ? 1U : 0U);
    writer.u32(efficiency.restorePriorityClass);
    return MessageKind::SetEfficiencyMode;
}

/// CloseConnection (#3066): the target, the socket's inode, then the IPv4 TCP endpoints (#3358): the local
/// address's 4 bytes and port, then the remote's.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const CloseConnectionRequest& close)
{
    writer.target(close.target);
    writer.u64(close.socketInode);
    const auto endpoint = [&writer](const std::array<std::uint8_t, 4>& address, std::uint16_t port)
    {
        for (const std::uint8_t byte : address)
        {
            writer.u8(byte);
        }
        writer.u16(port);
    };
    endpoint(close.tcpConnection.localAddress, close.tcpConnection.localPort);
    endpoint(close.tcpConnection.remoteAddress, close.tcpConnection.remotePort);
    return MessageKind::CloseConnection;
}

/// TaskManagerReplacement (#3072): the change, the path, then whether another program's Debugger may go.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const TaskManagerReplacementRequest& replacement)
{
    writer.u8(static_cast<std::uint8_t>(replacement.change));
    writer.text(replacement.path);
    writer.u8(replacement.replaceOther ? 1 : 0);
    return MessageKind::TaskManagerReplacement;
}

/// SessionAction (#3076): the action, the session's ID, then the confirmed sign-in: user, domain, sign-in time.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const SessionActionRequest& session)
{
    writer.u8(static_cast<std::uint8_t>(session.action));
    writer.u32(session.sessionId);
    writer.text(session.user);
    writer.text(session.domain);
    writer.u64(session.logonUsec);
    return MessageKind::SessionAction;
}

/// ThreadAction (#3069): the target, the thread's ID and creation time, then the action.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ThreadActionRequest& thread)
{
    writer.target(thread.target);
    writer.i32(thread.tid);
    writer.u64(thread.threadCreateTime);
    writer.u8(static_cast<std::uint8_t>(thread.action));
    return MessageKind::ThreadAction;
}

/// The reads that name only their target.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const EnvironmentRequest& environment)
{
    writer.target(environment.target);
    return MessageKind::ReadEnvironment;
}

[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ModulesRequest& modules)
{
    writer.target(modules.target);
    return MessageKind::ReadModules;
}

[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const OpenFilesRequest& files)
{
    writer.target(files.target);
    return MessageKind::ReadOpenFiles;
}

/// ReadOpenFilesBatch (#3578): the count, then each target.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const OpenFilesBatchRequest& batch)
{
    const std::size_t count = std::min(batch.targets.size(), MAX_OPEN_FILES_BATCH);
    writer.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i)
    {
        writer.target(batch.targets[i]);
    }
    return MessageKind::ReadOpenFilesBatch;
}

[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ConnectionsRequest& connections)
{
    writer.target(connections.target);
    return MessageKind::ReadConnections;
}

[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ThreadsRequest& threads)
{
    writer.target(threads.target);
    return MessageKind::ReadThreads;
}

/// ReadMemoryMap (#3665): the target only.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const MemoryMapRequest& map)
{
    writer.target(map.target);
    return MessageKind::ReadMemoryMap;
}

/// SetAffinity: the target, then the CPU set's word count and words.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const AffinityRequest& affinity)
{
    writer.target(affinity.target);
    const std::span<const std::uint64_t> words = affinity.cpus.words();
    writer.u32(static_cast<std::uint32_t>(words.size()));
    for (const std::uint64_t word : words)
    {
        writer.u64(word);
    }
    return MessageKind::SetAffinity;
}

} // namespace Detail

/// The frame of request @p requestId asking for @p request.
[[nodiscard]] inline std::vector<std::uint8_t> encodeRequest(std::uint32_t requestId, const Request& request)
{
    Detail::Writer writer;
    const MessageKind kind = std::visit([&writer](const auto& message) { return Detail::encodePayload(writer, message); }, request);
    return writer.frame(kind, requestId);
}

namespace Detail
{

/// A Signal request's payload: the target, then the signal, refused when past the last Signal.
[[nodiscard]] inline SignalRequest decodeSignalRequest(Reader& reader)
{
    const ProcessTarget target = reader.target();
    const std::uint8_t value = reader.u8();
    Signal signal = Signal::Terminate;
    if (value <= static_cast<std::uint8_t>(Signal::User2))
    {
        signal = static_cast<Signal>(value); // cast only once it is known to be in range
    }
    else
    {
        reader.fail();
    }
    return SignalRequest{.target = target, .signal = signal};
}

/// A SetPriority request's payload: the target, then the nice value.
[[nodiscard]] inline PriorityRequest decodePriorityRequest(Reader& reader)
{
    const ProcessTarget target = reader.target();
    return PriorityRequest{.target = target, .nice = reader.i32()};
}

/// A SetIoPriority request's payload: the target, the class (refused when past WindowsHint), then the level.
/// Each platform's actions refuse the other's classes, and a level out of range, again.
[[nodiscard]] inline IoPriorityRequest decodeIoPriorityRequest(Reader& reader)
{
    const ProcessTarget target = reader.target();
    const std::uint8_t value = reader.u8();
    IoPriorityClass ioClass = IoPriorityClass::BestEffort;
    if (value <= static_cast<std::uint8_t>(IoPriorityClass::WindowsHint))
    {
        ioClass = static_cast<IoPriorityClass>(value);
    }
    else
    {
        reader.fail();
    }
    const std::int32_t level = reader.i32();
    return IoPriorityRequest{.target = target, .ioClass = ioClass, .level = level};
}

/// A SetAffinity request's payload: the target, a word count (refused past MAX_CPUS worth), then the mask words.
[[nodiscard]] inline AffinityRequest decodeAffinityRequest(Reader& reader)
{
    const ProcessTarget target = reader.target();
    const std::uint32_t count = reader.u32();
    CpuAffinity cpus;
    if (count > CpuAffinity::MAX_CPUS / CpuAffinity::BITS_PER_WORD)
    {
        reader.fail();
    }
    for (std::uint32_t word = 0; reader.ok() && word < count; ++word)
    {
        const std::uint64_t bits = reader.u64();
        for (std::size_t bit = 0; bit < CpuAffinity::BITS_PER_WORD; ++bit)
        {
            if (((bits >> bit) & 1U) != 0)
            {
                cpus.set((static_cast<std::size_t>(word) * CpuAffinity::BITS_PER_WORD) + bit);
            }
        }
    }
    return AffinityRequest{.target = target, .cpus = std::move(cpus)};
}

/// A TaskManagerReplacement payload (#3072): the change (refused past Clear), the path and the flag. A Clear
/// with a path or replaceOther is refused: it has neither.
[[nodiscard]] inline TaskManagerReplacementRequest decodeTaskManagerReplacementRequest(Reader& reader)
{
    TaskManagerReplacementRequest request;
    const std::uint8_t change = reader.u8();
    request.path = reader.text();
    request.replaceOther = reader.flag();
    if (change > static_cast<std::uint8_t>(TaskManagerChange::Clear))
    {
        reader.fail();
        return request;
    }
    request.change = static_cast<TaskManagerChange>(change);
    if (request.change == TaskManagerChange::Clear && (!request.path.empty() || request.replaceOther))
    {
        reader.fail();
    }
    return request;
}

/// A SessionAction payload (#3076): the action, refused unless it is SignOut or Disconnect (Lock is
/// Linux's, which polkit authorizes, never the helper), the session's ID, then the confirmed sign-in's user,
/// domain and sign-in time (the helper, not the decoder, refuses a missing one).
[[nodiscard]] inline SessionActionRequest decodeSessionActionRequest(Reader& reader)
{
    SessionActionRequest request;
    const std::uint8_t action = reader.u8();
    request.sessionId = reader.u32();
    request.user = reader.text();
    request.domain = reader.text();
    request.logonUsec = reader.u64();
    if (action != static_cast<std::uint8_t>(SessionAction::SignOut) && action != static_cast<std::uint8_t>(SessionAction::Disconnect))
    {
        reader.fail();
        return request;
    }
    request.action = static_cast<SessionAction>(action);
    return request;
}

/// A ThreadAction payload (#3069): the target, the TID (refused unless positive), its creation time and the action
/// (refused past End). An unknown creation time (0) decodes, and the helper's actions refuse it.
[[nodiscard]] inline ThreadActionRequest decodeThreadActionRequest(Reader& reader)
{
    ThreadActionRequest request;
    request.target = reader.target();
    request.tid = reader.i32();
    request.threadCreateTime = reader.u64();
    const std::uint8_t action = reader.u8();
    if (request.tid <= 0 || action > static_cast<std::uint8_t>(ThreadAction::End))
    {
        reader.fail();
        return request;
    }
    request.action = static_cast<ThreadAction>(action);
    return request;
}

/// A CloseConnection payload (#3066): the target, the socket's inode, then the IPv4 TCP endpoints (#3358). Any
/// values decode: the helper's actions refuse a socket or connection the target doesn't hold.
[[nodiscard]] inline CloseConnectionRequest decodeCloseConnectionRequest(Reader& reader)
{
    CloseConnectionRequest request;
    request.target = reader.target();
    request.socketInode = reader.u64();
    const auto endpoint = [&reader](std::array<std::uint8_t, 4>& address, std::uint16_t& port)
    {
        for (std::uint8_t& byte : address)
        {
            byte = reader.u8();
        }
        port = reader.u16();
    };
    endpoint(request.tcpConnection.localAddress, request.tcpConnection.localPort);
    endpoint(request.tcpConnection.remoteAddress, request.tcpConnection.remotePort);
    return request;
}

/// A ReadOpenFilesBatch payload (#3578): a count, refused at 0 or past MAX_OPEN_FILES_BATCH, then each target.
[[nodiscard]] inline OpenFilesBatchRequest decodeOpenFilesBatchRequest(Reader& reader)
{
    OpenFilesBatchRequest request;
    const std::uint32_t count = reader.u32();
    if (count == 0 || count > MAX_OPEN_FILES_BATCH)
    {
        reader.fail();
        return request;
    }
    request.targets.reserve(count);
    for (std::uint32_t i = 0; reader.ok() && i < count; ++i)
    {
        request.targets.push_back(reader.target());
    }
    return request;
}

} // namespace Detail

/// The request in @p bytes (one whole frame), or why it was refused.
/// Each kind's payload is read by its Detail::decode*Request (or inline when it is one field); finish then
/// refuses the message if a read failed or bytes are left over.
[[nodiscard]] inline std::expected<Decoded<Request>, std::string> decodeRequest(std::span<const std::uint8_t> bytes)
{
    const auto frame = Detail::readFrame(bytes);
    if (!frame.has_value())
    {
        return std::unexpected(frame.error());
    }
    Detail::Reader reader(frame->payload);
    const std::uint32_t id = frame->requestId;
    switch (frame->kind)
    {
    case MessageKind::Hello:
        return Detail::finish<Request>(reader, id, Hello{.version = reader.u16()});
    case MessageKind::Signal:
        return Detail::finish<Request>(reader, id, Detail::decodeSignalRequest(reader));
    case MessageKind::SetPriority:
        return Detail::finish<Request>(reader, id, Detail::decodePriorityRequest(reader));
    case MessageKind::SetIoPriority:
        return Detail::finish<Request>(reader, id, Detail::decodeIoPriorityRequest(reader));
    case MessageKind::SetAffinity:
        return Detail::finish<Request>(reader, id, Detail::decodeAffinityRequest(reader));
    case MessageKind::SetEfficiencyMode:
        // Any class decodes (#3327): the helper's actions remember only a real priority class.
        return Detail::finish<Request>(
            reader, id, EfficiencyModeRequest{.target = reader.target(), .on = reader.flag(), .restorePriorityClass = reader.u32()});
    case MessageKind::CloseConnection:
        return Detail::finish<Request>(reader, id, Detail::decodeCloseConnectionRequest(reader));
    case MessageKind::TaskManagerReplacement:
        return Detail::finish<Request>(reader, id, Detail::decodeTaskManagerReplacementRequest(reader));
    case MessageKind::SessionAction:
        return Detail::finish<Request>(reader, id, Detail::decodeSessionActionRequest(reader));
    case MessageKind::ThreadAction:
        return Detail::finish<Request>(reader, id, Detail::decodeThreadActionRequest(reader));
    case MessageKind::ReadEnvironment:
        return Detail::finish<Request>(reader, id, EnvironmentRequest{.target = reader.target()});
    case MessageKind::ReadModules:
        return Detail::finish<Request>(reader, id, ModulesRequest{.target = reader.target()});
    case MessageKind::ReadOpenFiles:
        return Detail::finish<Request>(reader, id, OpenFilesRequest{.target = reader.target()});
    case MessageKind::ReadConnections:
        return Detail::finish<Request>(reader, id, ConnectionsRequest{.target = reader.target()});
    case MessageKind::ReadThreads:
        return Detail::finish<Request>(reader, id, ThreadsRequest{.target = reader.target()});
    case MessageKind::ReadOpenFilesBatch:
        return Detail::finish<Request>(reader, id, Detail::decodeOpenFilesBatchRequest(reader));
    case MessageKind::ReadMemoryMap:
        return Detail::finish<Request>(reader, id, MemoryMapRequest{.target = reader.target()});
    case MessageKind::HelloReply:
    case MessageKind::ActionResult:
    case MessageKind::EnvironmentResult:
    case MessageKind::ModulesResult:
    case MessageKind::OpenFilesResult:
    case MessageKind::ConnectionsResult:
    case MessageKind::ServiceActionResult:
    case MessageKind::ThreadsResult:
    case MessageKind::OpenFilesBatchResult:
    case MessageKind::MemoryMapResult:
    case MessageKind::Error:
        break;
    }
    return std::unexpected("Not a request this helper does");
}

namespace Detail
{

// One encoder per reply kind (#3043), as for the requests above; encodeResponse() visits the Response.

/// HelloReply: the helper's protocol version, and whether it accepted the client's.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const HelloReply& hello)
{
    writer.u16(hello.version);
    writer.u8(hello.accepted ? 1 : 0);
    return MessageKind::HelloReply;
}

/// ActionResult: success, the error message, the success note, then the priority class an Efficiency mode on replaced (#3327).
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ActionResult& result)
{
    writer.u8(result.success ? 1 : 0);
    writer.text(result.errorMessage);
    writer.text(result.successNote);
    writer.u32(result.priorityClassBefore);
    return MessageKind::ActionResult;
}

/// Error: why the helper couldn't answer.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ErrorReply& error)
{
    writer.text(error.message);
    return MessageKind::Error;
}

/// EnvironmentResult: status, truncated, count, then each variable's name and value; as many as fit a frame.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const EnvironmentReply& environment)
{
    const std::vector<EnvironmentVariable>& variables = environment.result.variables;
    constexpr std::size_t ROOM = listRoom(1 + 1 + 4); // status, truncated, count
    const std::size_t count =
        entriesThatFit(std::span(variables),
                       MAX_ENVIRONMENT_VARIABLES,
                       ROOM,
                       [](const EnvironmentVariable& variable) { return textBytes(variable.name) + textBytes(variable.value); });
    writer.u8(static_cast<std::uint8_t>(environment.result.status));
    writer.u8((environment.result.truncated || count < variables.size()) ? 1 : 0);
    writer.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i)
    {
        writer.text(variables[i].name);
        writer.text(variables[i].value);
    }
    return MessageKind::EnvironmentResult;
}

/// ModulesResult: status, detail, truncated, count; a module: path, base, size, version flag and version,
/// deleted. As many as fit a frame.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ModulesReply& modules)
{
    constexpr std::size_t ROOM = listRoom(1 + 4 + MAX_STRING_BYTES + 1 + 4);
    const std::size_t count = entriesThatFit(std::span(modules.result.modules),
                                             MAX_MODULES,
                                             ROOM,
                                             [](const ProcessModule& module) -> std::size_t
                                             { return textBytes(module.path) + 8 + 8 + 1 + (module.version.has_value() ? 8 : 0) + 1; });
    writer.u8(static_cast<std::uint8_t>(modules.result.status));
    writer.text(modules.result.detail);
    writer.u8((modules.result.truncated || count < modules.result.modules.size()) ? 1 : 0);
    writer.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i)
    {
        const ProcessModule& module = modules.result.modules[i];
        writer.text(module.path);
        writer.u64(module.baseAddress);
        writer.u64(module.sizeBytes);
        writer.u8(module.version.has_value() ? 1 : 0);
        if (module.version.has_value())
        {
            writer.u16(module.version->major);
            writer.u16(module.version->minor);
            writer.u16(module.version->build);
            writer.u16(module.version->revision);
        }
        writer.u8(module.deleted ? 1 : 0);
    }
    return MessageKind::ModulesResult;
}

/// OpenFilesResult: status, detail, three flags, count; a file: descriptor, kind, path, flag, flags, deleted.
/// As many files as fit in one frame: thousands of long paths could pass MAX_MESSAGE_BYTES, so the list
/// stops short of it and says it was cut, as a read past MAX_OPEN_FILES does.
/// The bytes one OpenFilesResult entry takes: descriptor, kind, path, flag, flags, deleted.
[[nodiscard]] inline std::size_t openFileBytes(const OpenFile& file) noexcept
{
    return 8 + 1 + textBytes(file.path) + 1 + 4 + 1;
}

/// The bytes one open-files result takes before its files: status, detail, three flags, count.
[[nodiscard]] inline std::size_t openFilesFixedBytes(const OpenFilesReadResult& result) noexcept
{
    return 1 + textBytes(result.detail) + 3 + 4;
}

/// One open-files result (an OpenFilesResult's payload, or one of an OpenFilesBatchResult's): status, detail, three
/// flags, then its first @p count files, saying it was cut when that is fewer than it holds.
inline void writeOpenFilesResult(Writer& writer, const OpenFilesReadResult& result, std::size_t count)
{
    writer.u8(static_cast<std::uint8_t>(result.status));
    writer.text(result.detail);
    writer.u8((result.truncated || count < result.files.size()) ? 1 : 0);
    writer.u8(result.namesIncomplete ? 1 : 0);
    writer.u8(result.hexDescriptors ? 1 : 0);
    writer.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i)
    {
        const OpenFile& file = result.files[i];
        writer.u64(file.descriptor);
        writer.u8(static_cast<std::uint8_t>(file.kind));
        writer.text(file.path);
        writer.u8(file.flags.has_value() ? 1 : 0);
        writer.u32(file.flags.value_or(0));
        writer.u8(file.deleted ? 1 : 0);
    }
}

[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const OpenFilesReply& files)
{
    constexpr std::size_t ROOM = listRoom(1 + 4 + MAX_STRING_BYTES + 3 + 4);
    writeOpenFilesResult(writer, files.result, entriesThatFit(std::span(files.result.files), MAX_OPEN_FILES, ROOM, openFileBytes));
    return MessageKind::OpenFilesResult;
}

/// OpenFilesBatchResult (#3578): a count, then each result as an OpenFilesResult's payload. As many whole results as
/// fit one frame, at most MAX_OPEN_FILES_BATCH; the first, when even it doesn't fit, is cut as an OpenFilesReply is,
/// so a reply to a request that named any target answers at least one, and the client asks again for the rest.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const OpenFilesBatchReply& batch)
{
    const std::size_t limit = std::min(batch.results.size(), MAX_OPEN_FILES_BATCH);
    std::size_t room = listRoom(4);  // after the count
    std::vector<std::size_t> counts; // how many of each result's files go
    counts.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i)
    {
        const OpenFilesReadResult& result = batch.results[i];
        const std::size_t fixed = openFilesFixedBytes(result);
        if (fixed > room)
        {
            break;
        }
        const std::size_t fit = entriesThatFit(std::span(result.files), MAX_OPEN_FILES, room - fixed, openFileBytes);
        if (i > 0 && fit < std::min(result.files.size(), MAX_OPEN_FILES))
        {
            break; // a later result goes whole, or in the next reply
        }
        std::size_t used = fixed;
        for (std::size_t file = 0; file < fit; ++file)
        {
            used += openFileBytes(result.files[file]);
        }
        room -= used;
        counts.push_back(fit);
    }
    writer.u32(static_cast<std::uint32_t>(counts.size()));
    for (std::size_t i = 0; i < counts.size(); ++i)
    {
        writeOpenFilesResult(writer, batch.results[i], counts[i]);
    }
    return MessageKind::OpenFilesBatchResult;
}

/// ConnectionsResult: status, detail, count; a socket: protocol, family, two endpoints, state. Every socket
/// takes the same bytes, so MAX_CONNECTIONS of them always fit a frame: the count is the only cap this list
/// needs (#2403).
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ConnectionsReply& connections)
{
    constexpr std::size_t ENDPOINT_BYTES = std::tuple_size_v<decltype(ConnectionEndpoint::address)> + 2;
    static_assert(MAX_CONNECTIONS * (1 + 1 + ENDPOINT_BYTES + ENDPOINT_BYTES + 1) <= listRoom(1 + 4 + MAX_STRING_BYTES + 4));
    writer.u8(static_cast<std::uint8_t>(connections.result.status));
    writer.text(connections.result.detail);
    const std::size_t count = std::min(connections.result.connections.size(), MAX_CONNECTIONS);
    writer.u32(static_cast<std::uint32_t>(count));
    const auto endpoint = [&writer](const ConnectionEndpoint& end)
    {
        for (const std::uint8_t byte : end.address)
        {
            writer.u8(byte);
        }
        writer.u16(end.port);
    };
    for (std::size_t i = 0; i < count; ++i)
    {
        const ProcessConnection& connection = connections.result.connections[i];
        writer.u8(static_cast<std::uint8_t>(connection.protocol));
        writer.u8(static_cast<std::uint8_t>(connection.family));
        endpoint(connection.local);
        endpoint(connection.remote);
        writer.u8(static_cast<std::uint8_t>(connection.state));
    }
    return MessageKind::ConnectionsResult;
}

/// ThreadsResult: status, detail, ticks per second, boot time, truncated, Windows priorities (v14), count; a
/// thread: TID, name, state, user and system ticks, nice, start ticks, and (v14) wait reason, suspend count and creation time.
/// As many threads as fit in one frame (names are at most 15 bytes on Linux, but the field is a string),
/// saying it was cut past them, as a read past MAX_THREADS does.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ThreadsReply& threads)
{
    constexpr std::size_t ROOM = listRoom(1 + 4 + MAX_STRING_BYTES + 8 + 8 + 1 + 1 + 4);
    const std::size_t count = entriesThatFit(
        std::span(threads.result.threads),
        MAX_THREADS,
        ROOM,
        [](const ProcessThread& thread) { return 4 + textBytes(thread.name) + 1 + 8 + 8 + 8 + 8 + textBytes(thread.waitReason) + 4 + 8; });
    writer.u8(static_cast<std::uint8_t>(threads.result.status));
    writer.text(threads.result.detail);
    writer.u64(static_cast<std::uint64_t>(threads.result.ticksPerSecond));
    writer.u64(threads.result.bootTimeEpoch);
    writer.u8((threads.result.truncated || count < threads.result.threads.size()) ? 1 : 0);
    writer.u8(threads.result.windowsPriorities ? 1 : 0);
    writer.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i)
    {
        const ProcessThread& thread = threads.result.threads[i];
        writer.u32(static_cast<std::uint32_t>(thread.tid));
        writer.text(thread.name);
        writer.u8(static_cast<std::uint8_t>(thread.state));
        writer.u64(thread.userTicks);
        writer.u64(thread.systemTicks);
        writer.u64(static_cast<std::uint64_t>(thread.nice));
        writer.u64(thread.startTicks);
        writer.text(thread.waitReason);
        writer.u32(thread.suspendCount);
        writer.u64(thread.createTime);
    }
    return MessageKind::ThreadsResult;
}

/// A MemoryMapResult mapping's flag bits, and its sizes in the order their presence bits name them (#3665, as v16).
namespace MemoryMapWire
{
inline constexpr std::uint8_t READABLE = 1U << 0U;
inline constexpr std::uint8_t WRITABLE = 1U << 1U;
inline constexpr std::uint8_t EXECUTABLE = 1U << 2U;
inline constexpr std::uint8_t SHARED = 1U << 3U;
inline constexpr std::uint8_t GUARD = 1U << 4U;
inline constexpr std::uint8_t RESERVED = 1U << 5U;
inline constexpr std::uint8_t DELETED = 1U << 6U;
inline constexpr std::uint8_t ALL_FLAGS = (1U << 7U) - 1U;

/// Each size field: bit i of a mapping's presence byte says whether SIZES[i] follows. Windows' reader fills none,
/// but the reply carries what any reader gives, so a fuller one later needs no new version.
inline constexpr std::array<std::optional<std::uint64_t> MemoryMapping::*, 8> SIZES{
    &MemoryMapping::residentBytes,
    &MemoryMapping::pssBytes,
    &MemoryMapping::sharedCleanBytes,
    &MemoryMapping::sharedDirtyBytes,
    &MemoryMapping::privateCleanBytes,
    &MemoryMapping::privateDirtyBytes,
    &MemoryMapping::swapBytes,
    &MemoryMapping::swapPssBytes,
};

/// The bytes one mapping takes on the wire: start, end, flags, kind, path, file offset, the presence byte, and
/// each size present.
[[nodiscard]] inline std::size_t bytesOf(const MemoryMapping& mapping) noexcept
{
    std::size_t sizes = 0;
    for (const auto field : SIZES)
    {
        sizes += (mapping.*field).has_value() ? 8 : 0;
    }
    return 8 + 8 + 1 + 1 + textBytes(mapping.path) + 8 + 1 + sizes;
}
} // namespace MemoryMapWire

/// MemoryMapResult (#3665): status, detail, truncated, count; a mapping: start, end, flag bits, kind, path, file
/// offset, a presence byte, then each size it names. As many as fit a frame, up to MAX_MEMORY_MAPPINGS, saying it
/// was cut past them, as a read past its own cap does.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const MemoryMapReply& map)
{
    constexpr std::size_t ROOM = listRoom(1 + 4 + MAX_STRING_BYTES + 1 + 4);
    const std::size_t count = entriesThatFit(std::span(map.result.mappings), MAX_MEMORY_MAPPINGS, ROOM, MemoryMapWire::bytesOf);
    writer.u8(static_cast<std::uint8_t>(map.result.status));
    writer.text(map.result.detail);
    writer.u8((map.result.truncated || count < map.result.mappings.size()) ? 1 : 0);
    writer.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i)
    {
        const MemoryMapping& mapping = map.result.mappings[i];
        writer.u64(mapping.start);
        writer.u64(mapping.end);
        const auto bit = [](bool on, std::uint8_t value)
        {
            return on ? value : std::uint8_t{0};
        };
        writer.u8(static_cast<std::uint8_t>(bit(mapping.readable, MemoryMapWire::READABLE) |
                                            bit(mapping.writable, MemoryMapWire::WRITABLE) |
                                            bit(mapping.executable, MemoryMapWire::EXECUTABLE) |
                                            bit(mapping.shared, MemoryMapWire::SHARED) | bit(mapping.guard, MemoryMapWire::GUARD) |
                                            bit(mapping.reserved, MemoryMapWire::RESERVED) | bit(mapping.deleted, MemoryMapWire::DELETED)));
        writer.u8(static_cast<std::uint8_t>(mapping.kind));
        writer.text(mapping.path);
        writer.u64(mapping.fileOffset);
        std::uint8_t present = 0;
        for (std::size_t field = 0; field < MemoryMapWire::SIZES.size(); ++field)
        {
            if ((mapping.*MemoryMapWire::SIZES[field]).has_value())
            {
                present = static_cast<std::uint8_t>(present | (1U << field));
            }
        }
        writer.u8(present);
        for (const auto field : MemoryMapWire::SIZES)
        {
            if (const std::optional<std::uint64_t>& size = mapping.*field; size.has_value())
            {
                writer.u64(*size);
            }
        }
    }
    return MessageKind::MemoryMapResult;
}

/// ServiceActionResult (#2728): flags (1: ok, 2: cancelled, 4: needs authorization, 8: authorization
/// unavailable), then the message.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ServiceActionReply& service)
{
    const ServiceActionResult& outcome = service.result;
    writer.u8(static_cast<std::uint8_t>((outcome.ok ? 1U : 0U) | (outcome.cancelled ? 2U : 0U) | (outcome.needsAuthorization ? 4U : 0U) |
                                        (outcome.authorizationUnavailable ? 8U : 0U)));
    writer.text(outcome.message);
    return MessageKind::ServiceActionResult;
}

} // namespace Detail

/// The frame of the response to request @p requestId.
[[nodiscard]] inline std::vector<std::uint8_t> encodeResponse(std::uint32_t requestId, const Response& response)
{
    Detail::Writer writer;
    const MessageKind kind = std::visit([&writer](const auto& message) { return Detail::encodePayload(writer, message); }, response);
    return writer.frame(kind, requestId);
}

namespace Detail
{

/// A HelloReply's payload: the helper's version, then whether it accepted ours.
[[nodiscard]] inline HelloReply decodeHelloReply(Reader& reader)
{
    const std::uint16_t version = reader.u16();
    return HelloReply{.version = version, .accepted = reader.flag()};
}

/// An ActionResult's payload: the success flag, the error message, the success note, then the priority class an
/// Efficiency mode on replaced (#3327).
[[nodiscard]] inline ActionResult decodeActionResult(Reader& reader)
{
    const bool success = reader.flag();
    std::string errorMessage = reader.text();
    std::string successNote = reader.text();
    const std::uint32_t priorityClassBefore = reader.u32();
    return ActionResult{
        .success = success,
        .errorMessage = std::move(errorMessage),
        .successNote = std::move(successNote),
        .priorityClassBefore = priorityClassBefore,
    };
}

/// An EnvironmentResult's payload: the status (refused past Failed), truncated, a count (refused past
/// MAX_ENVIRONMENT_VARIABLES), then each name and value.
[[nodiscard]] inline EnvironmentReply decodeEnvironmentReply(Reader& reader)
{
    const std::uint8_t value = reader.u8();
    EnvironmentReply reply;
    if (value <= static_cast<std::uint8_t>(EnvironmentReadStatus::Failed))
    {
        reply.result.status = static_cast<EnvironmentReadStatus>(value); // cast only once it is known to be in range
    }
    else
    {
        reader.fail();
    }
    reply.result.truncated = reader.flag();
    const std::uint32_t count = reader.u32();
    if (count > MAX_ENVIRONMENT_VARIABLES)
    {
        reader.fail();
    }
    for (std::uint32_t i = 0; reader.ok() && i < count; ++i)
    {
        std::string name = reader.text();
        std::string text = reader.text();
        reply.result.variables.push_back({.name = std::move(name), .value = std::move(text)});
    }
    return reply;
}

/// A ModulesResult's payload: the status (refused past Failed), detail, truncated, a count (refused past
/// MAX_MODULES), then each module's path, base, size, optional version and deleted flag.
[[nodiscard]] inline ModulesReply decodeModulesReply(Reader& reader)
{
    ModulesReply reply;
    const std::uint8_t value = reader.u8();
    if (value <= static_cast<std::uint8_t>(ModulesReadStatus::Failed))
    {
        reply.result.status = static_cast<ModulesReadStatus>(value);
    }
    else
    {
        reader.fail();
    }
    reply.result.detail = reader.text();
    reply.result.truncated = reader.flag();
    const std::uint32_t count = reader.u32();
    if (count > MAX_MODULES)
    {
        reader.fail();
    }
    for (std::uint32_t i = 0; reader.ok() && i < count; ++i)
    {
        ProcessModule module;
        module.path = reader.text();
        module.baseAddress = reader.u64();
        module.sizeBytes = reader.u64();
        if (reader.flag())
        {
            ModuleVersion version;
            version.major = reader.u16();
            version.minor = reader.u16();
            version.build = reader.u16();
            version.revision = reader.u16();
            module.version = version;
        }
        module.deleted = reader.flag();
        reply.result.modules.push_back(std::move(module));
    }
    return reply;
}

/// One OpenFilesResult entry: descriptor, kind (refused past Other), path, flags (always sent, kept only when
/// flagged present), then the deleted flag.
[[nodiscard]] inline OpenFile decodeOpenFile(Reader& reader)
{
    OpenFile file;
    file.descriptor = reader.u64();
    const std::uint8_t kind = reader.u8();
    if (kind <= static_cast<std::uint8_t>(OpenFileKind::Other))
    {
        file.kind = static_cast<OpenFileKind>(kind);
    }
    else
    {
        reader.fail();
    }
    file.path = reader.text();
    const bool hasFlags = reader.flag();
    const std::uint32_t flags = reader.u32();
    if (hasFlags)
    {
        file.flags = flags;
    }
    file.deleted = reader.flag();
    return file;
}

/// An OpenFilesResult's payload: the status (refused past Failed), detail, three flags, a count (refused past
/// MAX_OPEN_FILES), then each entry as decodeOpenFile reads it.
[[nodiscard]] inline OpenFilesReadResult decodeOpenFilesResult(Reader& reader)
{
    OpenFilesReadResult result;
    const std::uint8_t value = reader.u8();
    if (value <= static_cast<std::uint8_t>(OpenFilesReadStatus::Failed))
    {
        result.status = static_cast<OpenFilesReadStatus>(value);
    }
    else
    {
        reader.fail();
    }
    result.detail = reader.text();
    result.truncated = reader.flag();
    result.namesIncomplete = reader.flag();
    result.hexDescriptors = reader.flag();
    const std::uint32_t count = reader.u32();
    if (count > MAX_OPEN_FILES)
    {
        reader.fail();
    }
    for (std::uint32_t i = 0; reader.ok() && i < count; ++i)
    {
        result.files.push_back(decodeOpenFile(reader));
    }
    return result;
}

/// An OpenFilesBatchResult's payload (#3578): a count (refused past MAX_OPEN_FILES_BATCH), then each result as
/// decodeOpenFilesResult reads it.
[[nodiscard]] inline OpenFilesBatchReply decodeOpenFilesBatchReply(Reader& reader)
{
    OpenFilesBatchReply reply;
    const std::uint32_t count = reader.u32();
    if (count > MAX_OPEN_FILES_BATCH)
    {
        reader.fail();
    }
    for (std::uint32_t i = 0; reader.ok() && i < count; ++i)
    {
        reply.results.push_back(decodeOpenFilesResult(reader));
    }
    return reply;
}

/// A ThreadsResult's payload: the status (refused past Failed), detail, ticks per second (refused at 0 or
/// past INT64_MAX), boot time, truncated, Windows priorities, a count (refused past MAX_THREADS), then each thread.
[[nodiscard]] inline ThreadsReply decodeThreadsReply(Reader& reader)
{
    ThreadsReply reply;
    const std::uint8_t value = reader.u8();
    if (value <= static_cast<std::uint8_t>(ThreadsReadStatus::Failed))
    {
        reply.result.status = static_cast<ThreadsReadStatus>(value);
    }
    else
    {
        reader.fail();
    }
    reply.result.detail = reader.text();
    const std::uint64_t ticks = reader.u64();
    if (ticks == 0 || ticks > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        reader.fail();
    }
    reply.result.ticksPerSecond = static_cast<std::int64_t>(ticks);
    reply.result.bootTimeEpoch = reader.u64();
    reply.result.truncated = reader.flag();
    reply.result.windowsPriorities = reader.flag();
    const std::uint32_t count = reader.u32();
    if (count > MAX_THREADS)
    {
        reader.fail();
    }
    for (std::uint32_t i = 0; reader.ok() && i < count; ++i)
    {
        ProcessThread thread;
        thread.tid = static_cast<std::int32_t>(reader.u32());
        thread.name = reader.text();
        thread.state = static_cast<char>(reader.u8());
        thread.userTicks = reader.u64();
        thread.systemTicks = reader.u64();
        thread.nice = static_cast<std::int64_t>(reader.u64());
        thread.startTicks = reader.u64();
        thread.waitReason = reader.text();
        thread.suspendCount = reader.u32();
        thread.createTime = reader.u64();
        reply.result.threads.push_back(std::move(thread));
    }
    return reply;
}

/// A MemoryMapResult's payload (#3665, as v16's): the status (refused past Failed), detail, truncated, a count
/// (refused past MAX_MEMORY_MAPPINGS), then each mapping, refused when its end is before its start, it sets a flag
/// bit past DELETED, or its kind is past Other.
[[nodiscard]] inline MemoryMapReply decodeMemoryMapReply(Reader& reader)
{
    MemoryMapReply reply;
    const std::uint8_t value = reader.u8();
    if (value <= static_cast<std::uint8_t>(MemoryMapReadStatus::Failed))
    {
        reply.result.status = static_cast<MemoryMapReadStatus>(value);
    }
    else
    {
        reader.fail();
    }
    reply.result.detail = reader.text();
    reply.result.truncated = reader.flag();
    const std::uint32_t count = reader.u32();
    if (count > MAX_MEMORY_MAPPINGS)
    {
        reader.fail();
    }
    for (std::uint32_t i = 0; reader.ok() && i < count; ++i)
    {
        MemoryMapping mapping;
        mapping.start = reader.u64();
        mapping.end = reader.u64();
        if (mapping.end < mapping.start)
        {
            reader.fail();
        }
        const std::uint8_t flags = reader.u8();
        if ((flags & ~MemoryMapWire::ALL_FLAGS) != 0)
        {
            reader.fail();
        }
        mapping.readable = (flags & MemoryMapWire::READABLE) != 0;
        mapping.writable = (flags & MemoryMapWire::WRITABLE) != 0;
        mapping.executable = (flags & MemoryMapWire::EXECUTABLE) != 0;
        mapping.shared = (flags & MemoryMapWire::SHARED) != 0;
        mapping.guard = (flags & MemoryMapWire::GUARD) != 0;
        mapping.reserved = (flags & MemoryMapWire::RESERVED) != 0;
        mapping.deleted = (flags & MemoryMapWire::DELETED) != 0;
        const std::uint8_t kind = reader.u8();
        if (kind > static_cast<std::uint8_t>(MemoryMappingKind::Other))
        {
            reader.fail(); // never cast a byte past the enum: the mapping keeps its default kind
        }
        else
        {
            mapping.kind = static_cast<MemoryMappingKind>(kind);
        }
        mapping.path = reader.text();
        mapping.fileOffset = reader.u64();
        const std::uint8_t present = reader.u8();
        for (std::size_t field = 0; field < MemoryMapWire::SIZES.size(); ++field)
        {
            if ((present & (1U << field)) != 0)
            {
                mapping.*MemoryMapWire::SIZES[field] = reader.u64();
            }
        }
        reply.result.mappings.push_back(std::move(mapping));
    }
    return reply;
}

/// A ConnectionsResult's payload: the status (refused past Failed), detail, a count (refused past
/// MAX_CONNECTIONS), then each socket's protocol and family, two endpoints and state, each enum range-checked.
[[nodiscard]] inline ConnectionsReply decodeConnectionsReply(Reader& reader)
{
    ConnectionsReply reply;
    const std::uint8_t value = reader.u8();
    if (value <= static_cast<std::uint8_t>(ConnectionsReadStatus::Failed))
    {
        reply.result.status = static_cast<ConnectionsReadStatus>(value);
    }
    else
    {
        reader.fail();
    }
    reply.result.detail = reader.text();
    const std::uint32_t count = reader.u32();
    if (count > MAX_CONNECTIONS)
    {
        reader.fail();
    }
    const auto endpoint = [&reader]
    {
        ConnectionEndpoint end;
        for (std::uint8_t& byte : end.address)
        {
            byte = reader.u8();
        }
        end.port = reader.u16();
        return end;
    };
    for (std::uint32_t i = 0; reader.ok() && i < count; ++i)
    {
        ProcessConnection connection;
        const std::uint8_t protocol = reader.u8();
        const std::uint8_t family = reader.u8();
        if (protocol > static_cast<std::uint8_t>(ConnectionProtocol::Udp) || family > static_cast<std::uint8_t>(ConnectionFamily::IPv6))
        {
            reader.fail();
            break;
        }
        connection.protocol = static_cast<ConnectionProtocol>(protocol);
        connection.family = static_cast<ConnectionFamily>(family);
        connection.local = endpoint();
        connection.remote = endpoint();
        const std::uint8_t state = reader.u8();
        if (state > static_cast<std::uint8_t>(ConnectionState::Closing))
        {
            reader.fail();
            break;
        }
        connection.state = static_cast<ConnectionState>(state);
        reply.result.connections.push_back(connection);
    }
    return reply;
}

/// A ServiceActionResult's payload: the flags (refused past their four bits), then the message.
[[nodiscard]] inline ServiceActionReply decodeServiceActionReply(Reader& reader)
{
    ServiceActionReply reply;
    const std::uint8_t flags = reader.u8();
    if (flags > 15U)
    {
        reader.fail();
    }
    reply.result.ok = (flags & 1U) != 0;
    reply.result.cancelled = (flags & 2U) != 0;
    reply.result.needsAuthorization = (flags & 4U) != 0;
    reply.result.authorizationUnavailable = (flags & 8U) != 0;
    reply.result.message = reader.text();
    return reply;
}

} // namespace Detail

/// The response in @p bytes (one whole frame), or why it couldn't be read.
/// Each kind's payload is read by its Detail::decode* function (or inline when it is one field); finish then
/// refuses the message if a read failed or bytes are left over.
[[nodiscard]] inline std::expected<Decoded<Response>, std::string> decodeResponse(std::span<const std::uint8_t> bytes)
{
    const auto frame = Detail::readFrame(bytes);
    if (!frame.has_value())
    {
        return std::unexpected(frame.error());
    }
    Detail::Reader reader(frame->payload);
    const std::uint32_t id = frame->requestId;
    switch (frame->kind)
    {
    case MessageKind::HelloReply:
        return Detail::finish<Response>(reader, id, Detail::decodeHelloReply(reader));
    case MessageKind::ActionResult:
        return Detail::finish<Response>(reader, id, Detail::decodeActionResult(reader));
    case MessageKind::Error:
        return Detail::finish<Response>(reader, id, ErrorReply{.message = reader.text()});
    case MessageKind::EnvironmentResult:
        return Detail::finish<Response>(reader, id, Detail::decodeEnvironmentReply(reader));
    case MessageKind::ModulesResult:
        return Detail::finish<Response>(reader, id, Detail::decodeModulesReply(reader));
    case MessageKind::OpenFilesResult:
        return Detail::finish<Response>(reader, id, OpenFilesReply{.result = Detail::decodeOpenFilesResult(reader)});
    case MessageKind::ConnectionsResult:
        return Detail::finish<Response>(reader, id, Detail::decodeConnectionsReply(reader));
    case MessageKind::ServiceActionResult:
        return Detail::finish<Response>(reader, id, Detail::decodeServiceActionReply(reader));
    case MessageKind::ThreadsResult:
        return Detail::finish<Response>(reader, id, Detail::decodeThreadsReply(reader));
    case MessageKind::OpenFilesBatchResult:
        return Detail::finish<Response>(reader, id, Detail::decodeOpenFilesBatchReply(reader));
    case MessageKind::MemoryMapResult:
        return Detail::finish<Response>(reader, id, Detail::decodeMemoryMapReply(reader));
    case MessageKind::Hello:
    case MessageKind::Signal:
    case MessageKind::SetPriority:
    case MessageKind::SetIoPriority:
    case MessageKind::SetAffinity:
    case MessageKind::ReadEnvironment:
    case MessageKind::ReadModules:
    case MessageKind::ReadOpenFiles:
    case MessageKind::ReadConnections:
    case MessageKind::SetEfficiencyMode:
    case MessageKind::CloseConnection:
    case MessageKind::TaskManagerReplacement:
    case MessageKind::SessionAction:
    case MessageKind::ThreadAction:
    case MessageKind::ReadThreads:
    case MessageKind::ReadOpenFilesBatch:
    case MessageKind::ReadMemoryMap:
        break;
    }
    return std::unexpected("Not a response");
}

} // namespace Platform::Broker
