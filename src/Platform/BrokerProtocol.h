#pragma once

// The privileged-broker protocol (#2306; #2181, decision 4): what TaskSmack, never elevated itself, asks Windows'
// elevated helper (TaskSmackHelper.exe) over a private named pipe (#2311). Linux had a helper too, until #3580 (#3603)
// removed it: it never creates a broker now, so the messages only it served or only Linux sent are gone (v23). Pure
// encode and decode, no I/O, so it is unit-tested and fuzzed on every platform (tests/Platform/test_BrokerProtocol.cpp,
// tests/fuzz/fuzz_broker_protocol.cpp). This header holds the messages and the frame primitives; the encoders and
// decoders are BrokerProtocol.cpp's (#3786), so changing one recompiles that file, not every includer.
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
// (#2308), Process Details' reads (#2309), Replace Task Manager (#3072), Group: User's Disconnect and
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
#include <cstddef>
#include <cstdint>
#include <expected>
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
/// 26: what only the Linux helper used is gone from the requests left (#3784): CloseConnection's socket inode (a
///     Windows connection is named by its IPv4 TCP endpoints alone) and Signal's Hangup, Interrupt, Quit, User1 and
///     User2 (Windows has no POSIX signals). The kinds keep their numbers.
/// 27: a ModulesResult module ends with a version-not-read flag (#4373): its file isn't on a local drive, so the
///     helper didn't open it for its version.
inline constexpr std::uint16_t PROTOCOL_VERSION = 27;
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
    CloseConnection = 22,        ///< One of a process's IPv4 TCP connections reset, by its endpoints (#3358)
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

/// The signals a Signal request sends. v8's Hangup to User2 (#3063), the POSIX signals only Linux sent, went in v26
/// (#3784).
enum class Signal : std::uint8_t
{
    Terminate,
    Kill,
    Suspend,
    Resume,
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

/// Reset one of a process's IPv4 TCP connections (#3066, protocol v10; #3358, v20): IProcessActions::closeTcpConnection().
/// Its endpoints name it; the helper looks it up again after the identity check. The socket inode the Linux helper
/// closed by went in v26 (#3784).
struct CloseConnectionRequest
{
    ProcessTarget target;
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

// ---- The signed-in sessions' actions (#3076, protocol v13) ----

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
                   a.deleted == b.deleted && a.versionNotRead == b.versionNotRead;
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

// The codec's bodies live in BrokerProtocol.cpp (#3786), so a change to one no longer recompiles every file that
// includes this header. What stays here is what the tests build and read frames with: Writer, Reader and readFrame.

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

/// A frame's kind, request id and payload, as readFrame() found them.
struct Frame
{
    MessageKind kind = MessageKind::Error;
    std::uint32_t requestId = 0;
    std::span<const std::uint8_t> payload;
};

/// The frame in @p bytes: its kind, request id and payload, or why it isn't one.
[[nodiscard]] std::expected<Frame, std::string> readFrame(std::span<const std::uint8_t> bytes);

} // namespace Detail

/// The frame of request @p requestId asking for @p request.
[[nodiscard]] std::vector<std::uint8_t> encodeRequest(std::uint32_t requestId, const Request& request);

/// The request in @p bytes (one whole frame), or why it was refused.
/// Each kind's payload is read by its own decoder (BrokerProtocol.cpp); the message is then refused if a read
/// failed or bytes are left over.
[[nodiscard]] std::expected<Decoded<Request>, std::string> decodeRequest(std::span<const std::uint8_t> bytes);

/// The frame of the response to request @p requestId.
[[nodiscard]] std::vector<std::uint8_t> encodeResponse(std::uint32_t requestId, const Response& response);

/// The response in @p bytes (one whole frame), or why it couldn't be read.
/// Each kind's payload is read by its own decoder (BrokerProtocol.cpp); the message is then refused if a read
/// failed or bytes are left over.
[[nodiscard]] std::expected<Decoded<Response>, std::string> decodeResponse(std::span<const std::uint8_t> bytes);

} // namespace Platform::Broker
