#pragma once

// The privileged-broker protocol, one for Windows and Linux (#2306; #2181, decision 4). TaskSmack's UI
// never runs elevated; an elevated helper does a closed set of operations for it, asked over a private
// channel: a socketpair on Linux (pkexec, #2307), a named pipe on Windows (#2311). This header is the
// wire format both share. Pure encode and decode, no I/O, so it is unit-tested and fuzzed on every
// platform (tests/Platform/test_BrokerProtocol.cpp, tests/fuzz/fuzz_broker_protocol.cpp).
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
// else is refused as malformed. There is no "run a command" and no path operation, and every process
// request names its target by PID *and* start time (ProcessTarget), so the helper re-verifies the
// process and a reused PID can't redirect a request. Its kinds: the handshake, the process actions
// (#2308), Process Details' reads (#2309) and System Information's root-only facts (#2310).

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
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
inline constexpr std::uint16_t PROTOCOL_VERSION = 7;
/// The largest frame either side accepts, header included.
inline constexpr std::size_t MAX_MESSAGE_BYTES = std::size_t{8} << 20U;
/// The most processes one ReadProcessCounters asks about (#2728); a sampler with more asks for the first.
inline constexpr std::size_t MAX_COUNTER_TARGETS = std::size_t{1} << 14U;
/// The most socket inodes one ProcessCountersReply carries, over all its entries (#2728); an entry past it
/// goes back without its sockets.
inline constexpr std::size_t MAX_COUNTER_SOCKET_INODES = std::size_t{1} << 18U;
/// The most PIDs one ListHiddenProcesses says TaskSmack already sees (#2728); a request with more is refused.
inline constexpr std::size_t MAX_VISIBLE_PIDS = std::size_t{1} << 18U;
/// The most hidden processes one HiddenProcessesReply carries; past it, or past a frame, it says it was cut.
inline constexpr std::size_t MAX_HIDDEN_PROCESSES = std::size_t{1} << 14U;
/// The longest string field (an error message, a note).
inline constexpr std::size_t MAX_STRING_BYTES = std::size_t{64} << 10U;
/// The most environment variables an EnvironmentReply carries; a read with more keeps the first. Fewer
/// fit when they are long: every list reply stops short of MAX_MESSAGE_BYTES and says it was cut (#2403).
inline constexpr std::size_t MAX_ENVIRONMENT_VARIABLES = std::size_t{1} << 16U;
/// The most modules a ModulesReply carries; a read with more keeps the first.
inline constexpr std::size_t MAX_MODULES = std::size_t{1} << 16U;
/// The most sockets a ConnectionsReply carries; a read with more keeps the first.
inline constexpr std::size_t MAX_CONNECTIONS = std::size_t{1} << 16U;
/// The largest firmware file a FirmwareFileReply carries; SMBIOS tables are a few KiB to some tens.
inline constexpr std::size_t MAX_FIRMWARE_FILE_BYTES = std::size_t{1} << 20U;
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
    ServiceAction = 20,         ///< Start, stop or restart a service, or set its start type (#2728)
    ReadEnvironment = 32,       ///< Another user's process's environment, for Process Details (#2309)
    ReadModules = 33,           ///< Its loaded modules (#2309)
    ReadOpenFiles = 34,         ///< Its open files (#2309)
    ReadConnections = 35,       ///< Its TCP and UDP sockets (#2309)
    ReadMemoryRollup = 36,      ///< Its PSS and USS (#2309)
    ReadFirmwareFile = 37,      ///< One of System Information's root-only firmware files (#2310)
    ReadCrashes = 38,           ///< Recent crashes, every user's (#2310)
    ReadProcessCounters = 39,   ///< Many processes' FD counts and I/O bytes at once, for the sampler (#2728)
    ListHiddenProcesses = 40,   ///< The processes hidepid hides from TaskSmack, as their /proc files' text (#2728)
    ActionResult = 48,          ///< The answer to a process action
    EnvironmentResult = 49,     ///< The answer to ReadEnvironment
    ModulesResult = 50,         ///< The answer to ReadModules
    OpenFilesResult = 51,       ///< The answer to ReadOpenFiles
    ConnectionsResult = 52,     ///< The answer to ReadConnections
    MemoryRollupResult = 53,    ///< The answer to ReadMemoryRollup
    FirmwareFileResult = 54,    ///< The answer to ReadFirmwareFile
    CrashesResult = 55,         ///< The answer to ReadCrashes
    ProcessCountersResult = 56, ///< The answer to ReadProcessCounters
    HiddenProcessesResult = 57, ///< The answer to ListHiddenProcesses
    ServiceActionResult = 58,   ///< The answer to ServiceAction
    Error = 63,                 ///< A request the helper refused to read (malformed, or a kind it doesn't do)
};

/// The signals a Signal request sends.
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
    /// The user chose Administrator for this session (#2728): the Linux helper drops its idle exit and
    /// serves until TaskSmack closes the socket. A Hello again mid-session can set it, so a helper an
    /// "Authenticate and retry" started needs no second prompt. Windows restarts elevated instead, and
    /// its pipe helper ignores it.
    bool forSession = false;
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

/// The answer to OpenFilesRequest: the read's result, as IProcessOpenFilesReader returns it.
struct OpenFilesReply
{
    OpenFilesReadResult result;

    friend bool operator==(const OpenFilesReply& left, const OpenFilesReply& right)
    {
        const auto same = [](const OpenFile& a, const OpenFile& b)
        {
            return a.descriptor == b.descriptor && a.kind == b.kind && a.path == b.path && a.flags == b.flags && a.deleted == b.deleted;
        };
        return left.result.status == right.result.status && left.result.detail == right.result.detail &&
               left.result.truncated == right.result.truncated && left.result.namesIncomplete == right.result.namesIncomplete &&
               left.result.hexDescriptors == right.result.hexDescriptors && std::ranges::equal(left.result.files, right.result.files, same);
    }
};

/// List a process's TCP and UDP sockets (#2309), as the helper reads them with its capabilities.
struct ConnectionsRequest
{
    ProcessTarget target;
    friend bool operator==(const ConnectionsRequest&, const ConnectionsRequest&) = default;
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

/// Read many processes' FD counts and I/O bytes at once (#2728): the processes the sampler's own read was
/// refused (other users', without CAP_DAC_READ_SEARCH and CAP_SYS_PTRACE), every refresh, in one round
/// trip. At most MAX_COUNTER_TARGETS. With @c withSockets, each entry also lists the process's socket
/// inodes, which the sampler can't read from another user's /proc/<pid>/fd links to attribute its connections.
struct ProcessCountersRequest
{
    std::vector<ProcessTarget> targets;
    bool withSockets = false;
    friend bool operator==(const ProcessCountersRequest&, const ProcessCountersRequest&) = default;
};

/// One process's counters in a ProcessCountersReply: each figure with whether it was read.
struct ProcessCountersEntry
{
    ProcessTarget target;
    bool handleCountRead = false; ///< /proc/<pid>/fd was listed
    std::int32_t handleCount = 0;
    bool ioRead = false; ///< /proc/<pid>/io gave read_bytes and write_bytes
    std::uint64_t readBytes = 0;
    std::uint64_t writeBytes = 0;
    bool socketsRead = false; ///< Asked for, and every /proc/<pid>/fd link was read
    /// Its sockets' inodes, when socketsRead
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    std::vector<std::uint64_t> socketInodes{};
    friend bool operator==(const ProcessCountersEntry&, const ProcessCountersEntry&) = default;
};

/// The answer to ProcessCountersRequest: an entry for each target still running with that start time, in
/// the request's order; a process that has exited, or whose PID now names another, has none.
struct ProcessCountersReply
{
    std::vector<ProcessCountersEntry> entries;
    friend bool operator==(const ProcessCountersReply&, const ProcessCountersReply&) = default;
};

/// Start, stop or restart the service @c unit, or set its start type (#2728): what Linux's helper does as root
/// over the system bus connection it opened before its sandbox, so systemd asks for no password. @c unit
/// is a unit name the helper checks (a .service, systemd's characters, at most 255 bytes); @c startType is
/// SetStartType's.
struct ServiceActionRequest
{
    ServiceAction action = ServiceAction::Start;
    std::string unit;
    ServiceStartType startType = ServiceStartType::Unknown;
    friend bool operator==(const ServiceActionRequest&, const ServiceActionRequest&) = default;
};

/// The answer to ServiceActionRequest: the action's result, as TaskSmack's own call would have given it.
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

/// List the processes hidepid hides from TaskSmack (#2728): every process the helper sees whose PID isn't in
/// @c visiblePids (the ones TaskSmack's own listing found), as the text of a fixed set of its /proc/<pid>
/// files, which TaskSmack parses as it parses its own. At most MAX_VISIBLE_PIDS.
struct HiddenProcessesRequest
{
    std::vector<std::int32_t> visiblePids;
    friend bool operator==(const HiddenProcessesRequest&, const HiddenProcessesRequest&) = default;
};

/// One hidden process: its identity (the PID and the start time its stat gave) and its files' text, each
/// at most MAX_STRING_BYTES. Only these files, never a path the request names.
// NOLINTBEGIN(readability-redundant-member-init) - {} lets designated initializers omit a field without a warning
struct HiddenProcess
{
    ProcessTarget target{};
    std::string stat{};
    std::string statm{};
    std::string status{};
    std::optional<std::string> cmdline{}; ///< nullopt when it couldn't be read; empty for a kernel thread
    std::string io{};                     ///< Empty when unreadable
    std::string cgroup{};
    friend bool operator==(const HiddenProcess&, const HiddenProcess&) = default;
};
// NOLINTEND(readability-redundant-member-init)

/// The answer to HiddenProcessesRequest. @c truncated: more were hidden than MAX_HIDDEN_PROCESSES or a
/// frame holds.
struct HiddenProcessesReply
{
    std::vector<HiddenProcess> processes;
    bool truncated = false;
    friend bool operator==(const HiddenProcessesReply&, const HiddenProcessesReply&) = default;
};

/// Read a process's PSS and USS (#2309), as the helper reads its smaps_rollup with its capabilities.
struct MemoryRollupRequest
{
    ProcessTarget target;
    friend bool operator==(const MemoryRollupRequest&, const MemoryRollupRequest&) = default;
};

/// The answer to MemoryRollupRequest: the figures, or why there are none.
struct MemoryRollupReply
{
    MemoryRollupRead result = std::unexpected(MemoryRollupFailure::Unavailable);

    friend bool operator==(const MemoryRollupReply& left, const MemoryRollupReply& right)
    {
        if (left.result.has_value() != right.result.has_value())
        {
            return false;
        }
        if (!left.result.has_value())
        {
            return left.result.error() == right.result.error();
        }
        return left.result->pssBytes == right.result->pssBytes && left.result->ussBytes == right.result->ussBytes &&
               left.result->swapPssBytes == right.result->swapPssBytes;
    }
};

/// The root-only firmware files System Information reads through the helper (#2310). The helper reads
/// these and nothing else: a request names one of them, never a path.
enum class FirmwareFile : std::uint8_t
{
    SmbiosEntryPoint, ///< /sys/firmware/dmi/tables/smbios_entry_point
    SmbiosTable,      ///< /sys/firmware/dmi/tables/DMI
    ProductSerial,    ///< /sys/class/dmi/id/product_serial
    ProductUuid,      ///< /sys/class/dmi/id/product_uuid
    BoardSerial,      ///< /sys/class/dmi/id/board_serial
};

/// Read one of the firmware files (#2310), as the helper reads it with its capabilities.
struct FirmwareFileRequest
{
    FirmwareFile file = FirmwareFile::SmbiosTable;
    friend bool operator==(const FirmwareFileRequest&, const FirmwareFileRequest&) = default;
};

enum class FirmwareFileStatus : std::uint8_t
{
    Ok,
    NotFound,   ///< The machine has no such file (no DMI, or a VM without the field)
    Unreadable, ///< It exists but couldn't be read, or is larger than MAX_FIRMWARE_FILE_BYTES
};

/// The answer to FirmwareFileRequest: the file's bytes when the status is Ok.
struct FirmwareFileReply
{
    FirmwareFileStatus status = FirmwareFileStatus::NotFound;
    std::vector<std::uint8_t> bytes;
    friend bool operator==(const FirmwareFileReply&, const FirmwareFileReply&) = default;
};

/// Read System Information's Recent crashes (#2310) as the helper reads them with its capabilities: systemd-coredump's
/// journal entries and core files of every user, not only this one's.
struct CrashesRequest
{
    friend bool operator==(const CrashesRequest&, const CrashesRequest&) = default;
};

/// The answer to CrashesRequest: the Linux facts of CrashesInfo (the Windows-only fields stay empty), at
/// most CRASH_LIST_MAX events.
struct CrashesReply
{
    CrashesInfo info;

    friend bool operator==(const CrashesReply& left, const CrashesReply& right)
    {
        const auto same = [](const CrashEvent& a, const CrashEvent& b)
        {
            return a.unixSeconds == b.unixSeconds && a.application == b.application && a.pid == b.pid && a.uid == b.uid &&
                   a.coreBytes == b.coreBytes && a.coreKept == b.coreKept && a.signal == b.signal && a.executable == b.executable;
        };
        const CrashesInfo& l = left.info;
        const CrashesInfo& r = right.info;
        return l.available == r.available && l.family == r.family && l.listed == r.listed && l.accessDenied == r.accessDenied &&
               l.unavailableReason == r.unavailableReason && l.capped == r.capped && l.journalRead == r.journalRead &&
               l.journalUnavailableReason == r.journalUnavailableReason && l.otherUsersHidden == r.otherUsersHidden &&
               std::ranges::equal(l.events, r.events, same);
    }
};

/// The answer to a process action: ProcessActionResult's fields.
struct ActionResult
{
    bool success = false;
    std::string errorMessage;
    std::string successNote;
    friend bool operator==(const ActionResult&, const ActionResult&) = default;

    [[nodiscard]] static ActionResult from(const ProcessActionResult& result)
    {
        return {.success = result.success, .errorMessage = result.errorMessage, .successNote = result.successNote};
    }
    [[nodiscard]] ProcessActionResult toProcessActionResult() const
    {
        return success ? ProcessActionResult::ok(successNote) : ProcessActionResult::error(errorMessage);
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
                             MemoryRollupRequest,
                             FirmwareFileRequest,
                             CrashesRequest,
                             ProcessCountersRequest,
                             HiddenProcessesRequest,
                             ServiceActionRequest>;
using Response = std::variant<HelloReply,
                              ActionResult,
                              ErrorReply,
                              EnvironmentReply,
                              ModulesReply,
                              OpenFilesReply,
                              ConnectionsReply,
                              MemoryRollupReply,
                              FirmwareFileReply,
                              CrashesReply,
                              ProcessCountersReply,
                              HiddenProcessesReply,
                              ServiceActionReply>;

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
    /// A length and raw bytes, up to @p limit (more is cut off, as the reader would refuse it)
    void blob(std::span<const std::uint8_t> value, std::size_t limit)
    {
        const std::span<const std::uint8_t> capped = value.first(std::min(value.size(), limit));
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
    std::vector<std::uint8_t> blob(std::size_t limit)
    {
        const std::uint32_t size = u32();
        if (!m_Ok || size > limit || m_Bytes.size() - m_At < size)
        {
            m_Ok = false;
            return {};
        }
        const auto start = m_Bytes.begin() + static_cast<std::ptrdiff_t>(m_At);
        std::vector<std::uint8_t> value(start, start + static_cast<std::ptrdiff_t>(size));
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

/// Hello: the client's protocol version, and whether the helper is held for the session (#2728).
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const Hello& hello)
{
    writer.u16(hello.version);
    writer.u8(hello.forSession ? 1 : 0);
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

[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ConnectionsRequest& connections)
{
    writer.target(connections.target);
    return MessageKind::ReadConnections;
}

[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const MemoryRollupRequest& rollup)
{
    writer.target(rollup.target);
    return MessageKind::ReadMemoryRollup;
}

/// ReadProcessCounters: whether to read sockets, then the count and up to MAX_COUNTER_TARGETS targets.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ProcessCountersRequest& counters)
{
    writer.u8(counters.withSockets ? 1U : 0U);
    const std::size_t count = std::min(counters.targets.size(), MAX_COUNTER_TARGETS);
    writer.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i)
    {
        writer.target(counters.targets[i]);
    }
    return MessageKind::ReadProcessCounters;
}

/// ServiceAction (#2728): the action, the unit's name, then the start type.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ServiceActionRequest& service)
{
    writer.u8(static_cast<std::uint8_t>(service.action));
    writer.text(service.unit);
    writer.u8(static_cast<std::uint8_t>(service.startType));
    return MessageKind::ServiceAction;
}

/// ListHiddenProcesses (#2728): the count, then up to MAX_VISIBLE_PIDS PIDs the client already lists.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const HiddenProcessesRequest& hidden)
{
    const std::size_t count = std::min(hidden.visiblePids.size(), MAX_VISIBLE_PIDS);
    writer.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i)
    {
        writer.i32(hidden.visiblePids[i]);
    }
    return MessageKind::ListHiddenProcesses;
}

/// ReadFirmwareFile: which file.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const FirmwareFileRequest& firmware)
{
    writer.u8(static_cast<std::uint8_t>(firmware.file));
    return MessageKind::ReadFirmwareFile;
}

/// ReadCrashes: no payload.
[[nodiscard]] inline MessageKind encodePayload(Writer& /*writer*/, const CrashesRequest& /*crashes*/)
{
    return MessageKind::ReadCrashes;
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
    if (value <= static_cast<std::uint8_t>(Signal::Resume))
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

/// A SetIoPriority request's payload: the target, the class (refused when past Idle), then the level.
[[nodiscard]] inline IoPriorityRequest decodeIoPriorityRequest(Reader& reader)
{
    const ProcessTarget target = reader.target();
    const std::uint8_t value = reader.u8();
    IoPriorityClass ioClass = IoPriorityClass::BestEffort;
    if (value <= static_cast<std::uint8_t>(IoPriorityClass::Idle))
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

/// A ReadProcessCounters payload: whether sockets are wanted (0 or 1), the count (at most
/// MAX_COUNTER_TARGETS), then the targets.
[[nodiscard]] inline ProcessCountersRequest decodeProcessCountersRequest(Reader& reader)
{
    ProcessCountersRequest request;
    const std::uint8_t withSockets = reader.u8();
    if (withSockets > 1U)
    {
        reader.fail();
    }
    request.withSockets = withSockets == 1U;
    const std::uint32_t count = reader.u32();
    if (count > MAX_COUNTER_TARGETS)
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

/// A ServiceAction payload: the action (refused past SetStartType), the unit, and the start type (refused
/// past System).
[[nodiscard]] inline ServiceActionRequest decodeServiceActionRequest(Reader& reader)
{
    ServiceActionRequest request;
    const std::uint8_t action = reader.u8();
    request.unit = reader.text();
    const std::uint8_t startType = reader.u8();
    if (action > static_cast<std::uint8_t>(ServiceAction::SetStartType) || startType > static_cast<std::uint8_t>(ServiceStartType::System))
    {
        reader.fail();
        return request;
    }
    request.action = static_cast<ServiceAction>(action);
    request.startType = static_cast<ServiceStartType>(startType);
    return request;
}

/// A ListHiddenProcesses payload: the count (refused past MAX_VISIBLE_PIDS), then the visible PIDs.
[[nodiscard]] inline HiddenProcessesRequest decodeHiddenProcessesRequest(Reader& reader)
{
    HiddenProcessesRequest request;
    const std::uint32_t count = reader.u32();
    if (count > MAX_VISIBLE_PIDS)
    {
        reader.fail();
        return request;
    }
    request.visiblePids.reserve(count);
    for (std::uint32_t i = 0; reader.ok() && i < count; ++i)
    {
        request.visiblePids.push_back(reader.i32());
    }
    return request;
}

/// A ReadFirmwareFile request's payload: the file, refused when past BoardSerial.
[[nodiscard]] inline FirmwareFileRequest decodeFirmwareFileRequest(Reader& reader)
{
    const std::uint8_t file = reader.u8();
    FirmwareFileRequest firmware;
    if (file <= static_cast<std::uint8_t>(FirmwareFile::BoardSerial))
    {
        firmware.file = static_cast<FirmwareFile>(file);
    }
    else
    {
        reader.fail();
    }
    return firmware;
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
        return Detail::finish<Request>(reader, id, Hello{.version = reader.u16(), .forSession = reader.flag()});
    case MessageKind::Signal:
        return Detail::finish<Request>(reader, id, Detail::decodeSignalRequest(reader));
    case MessageKind::SetPriority:
        return Detail::finish<Request>(reader, id, Detail::decodePriorityRequest(reader));
    case MessageKind::SetIoPriority:
        return Detail::finish<Request>(reader, id, Detail::decodeIoPriorityRequest(reader));
    case MessageKind::SetAffinity:
        return Detail::finish<Request>(reader, id, Detail::decodeAffinityRequest(reader));
    case MessageKind::ReadEnvironment:
        return Detail::finish<Request>(reader, id, EnvironmentRequest{.target = reader.target()});
    case MessageKind::ReadModules:
        return Detail::finish<Request>(reader, id, ModulesRequest{.target = reader.target()});
    case MessageKind::ReadOpenFiles:
        return Detail::finish<Request>(reader, id, OpenFilesRequest{.target = reader.target()});
    case MessageKind::ReadConnections:
        return Detail::finish<Request>(reader, id, ConnectionsRequest{.target = reader.target()});
    case MessageKind::ReadMemoryRollup:
        return Detail::finish<Request>(reader, id, MemoryRollupRequest{.target = reader.target()});
    case MessageKind::ReadProcessCounters:
        return Detail::finish<Request>(reader, id, Detail::decodeProcessCountersRequest(reader));
    case MessageKind::ListHiddenProcesses:
        return Detail::finish<Request>(reader, id, Detail::decodeHiddenProcessesRequest(reader));
    case MessageKind::ServiceAction:
        return Detail::finish<Request>(reader, id, Detail::decodeServiceActionRequest(reader));
    case MessageKind::ReadFirmwareFile:
        return Detail::finish<Request>(reader, id, Detail::decodeFirmwareFileRequest(reader));
    case MessageKind::ReadCrashes:
        return Detail::finish<Request>(reader, id, CrashesRequest{});
    case MessageKind::HelloReply:
    case MessageKind::ActionResult:
    case MessageKind::EnvironmentResult:
    case MessageKind::ModulesResult:
    case MessageKind::OpenFilesResult:
    case MessageKind::ConnectionsResult:
    case MessageKind::MemoryRollupResult:
    case MessageKind::FirmwareFileResult:
    case MessageKind::CrashesResult:
    case MessageKind::ProcessCountersResult:
    case MessageKind::HiddenProcessesResult:
    case MessageKind::ServiceActionResult:
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

/// ActionResult: success, the error message, then the success note.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ActionResult& result)
{
    writer.u8(result.success ? 1 : 0);
    writer.text(result.errorMessage);
    writer.text(result.successNote);
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
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const OpenFilesReply& files)
{
    constexpr std::size_t ROOM = listRoom(1 + 4 + MAX_STRING_BYTES + 3 + 4);
    const std::size_t count = entriesThatFit(
        std::span(files.result.files), MAX_OPEN_FILES, ROOM, [](const OpenFile& file) { return 8 + 1 + textBytes(file.path) + 1 + 4 + 1; });
    writer.u8(static_cast<std::uint8_t>(files.result.status));
    writer.text(files.result.detail);
    writer.u8((files.result.truncated || count < files.result.files.size()) ? 1 : 0);
    writer.u8(files.result.namesIncomplete ? 1 : 0);
    writer.u8(files.result.hexDescriptors ? 1 : 0);
    writer.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i)
    {
        const OpenFile& file = files.result.files[i];
        writer.u64(file.descriptor);
        writer.u8(static_cast<std::uint8_t>(file.kind));
        writer.text(file.path);
        writer.u8(file.flags.has_value() ? 1 : 0);
        writer.u32(file.flags.value_or(0));
        writer.u8(file.deleted ? 1 : 0);
    }
    return MessageKind::OpenFilesResult;
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

/// MemoryRollupResult: 0 when the figures follow, else 1 + the failure; then PSS, USS and swap PSS.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const MemoryRollupReply& rollup)
{
    writer.u8(rollup.result.has_value() ? 0 : static_cast<std::uint8_t>(1 + static_cast<std::uint8_t>(rollup.result.error())));
    const MemoryRollup figures = rollup.result.value_or(MemoryRollup{});
    writer.u64(figures.pssBytes);
    writer.u64(figures.ussBytes);
    writer.u64(figures.swapPssBytes);
    return MessageKind::MemoryRollupResult;
}

/// ProcessCountersResult: the count; an entry: the target, a flags byte (1: the handle count was read, 2: the
/// I/O was, 4: the sockets were), the count, both byte figures, then the socket inodes' count and the
/// inodes. MAX_COUNTER_TARGETS entries and MAX_COUNTER_SOCKET_INODES inodes between them fit a frame with
/// room to spare; an entry whose sockets would pass that goes without them.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const ProcessCountersReply& counters)
{
    static_assert((MAX_COUNTER_TARGETS * (4 + 8 + 1 + 4 + 8 + 8 + 4)) + (MAX_COUNTER_SOCKET_INODES * 8) <= listRoom(4));
    const std::size_t count = std::min(counters.entries.size(), MAX_COUNTER_TARGETS);
    writer.u32(static_cast<std::uint32_t>(count));
    std::size_t inodesLeft = MAX_COUNTER_SOCKET_INODES;
    for (std::size_t i = 0; i < count; ++i)
    {
        const ProcessCountersEntry& entry = counters.entries[i];
        const bool sockets = entry.socketsRead && entry.socketInodes.size() <= inodesLeft;
        writer.target(entry.target);
        writer.u8(static_cast<std::uint8_t>((entry.handleCountRead ? 1U : 0U) | (entry.ioRead ? 2U : 0U) | (sockets ? 4U : 0U)));
        writer.i32(entry.handleCount);
        writer.u64(entry.readBytes);
        writer.u64(entry.writeBytes);
        writer.u32(static_cast<std::uint32_t>(sockets ? entry.socketInodes.size() : 0U));
        if (sockets)
        {
            inodesLeft -= entry.socketInodes.size();
            for (const std::uint64_t inode : entry.socketInodes)
            {
                writer.u64(inode);
            }
        }
    }
    return MessageKind::ProcessCountersResult;
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

/// HiddenProcessesResult (#2728): truncated, count; a process: its target, five texts and the command
/// line's flag and text. As many processes as fit in one frame, and it says when it was cut.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const HiddenProcessesReply& hidden)
{
    constexpr std::size_t ROOM = listRoom(1 + 4);
    const std::size_t count = entriesThatFit(std::span(hidden.processes),
                                             MAX_HIDDEN_PROCESSES,
                                             ROOM,
                                             [](const HiddenProcess& process)
                                             {
                                                 return 4 + 8 + textBytes(process.stat) + textBytes(process.statm) +
                                                        textBytes(process.status) + 1 + textBytes(process.cmdline.value_or(std::string{})) +
                                                        textBytes(process.io) + textBytes(process.cgroup);
                                             });
    writer.u8((hidden.truncated || count < hidden.processes.size()) ? 1 : 0);
    writer.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i)
    {
        const HiddenProcess& process = hidden.processes[i];
        writer.target(process.target);
        writer.text(process.stat);
        writer.text(process.statm);
        writer.text(process.status);
        writer.u8(process.cmdline.has_value() ? 1 : 0);
        writer.text(process.cmdline.value_or(std::string{}));
        writer.text(process.io);
        writer.text(process.cgroup);
    }
    return MessageKind::HiddenProcessesResult;
}

/// FirmwareFileResult: the status, then up to MAX_FIRMWARE_FILE_BYTES of the file.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const FirmwareFileReply& firmware)
{
    writer.u8(static_cast<std::uint8_t>(firmware.status));
    writer.blob(firmware.bytes, MAX_FIRMWARE_FILE_BYTES);
    return MessageKind::FirmwareFileResult;
}

/// CrashesResult: the listing's flags and reasons, the count, then up to CRASH_LIST_MAX events.
[[nodiscard]] inline MessageKind encodePayload(Writer& writer, const CrashesReply& crashes)
{
    const CrashesInfo& info = crashes.info;
    writer.u8(info.available ? 1 : 0);
    writer.u8(static_cast<std::uint8_t>(info.family));
    writer.u8(info.listed ? 1 : 0);
    writer.u8(info.accessDenied ? 1 : 0);
    writer.text(info.unavailableReason);
    writer.u8(info.capped ? 1 : 0);
    writer.u8(info.journalRead ? 1 : 0);
    writer.text(info.journalUnavailableReason);
    writer.u8(info.otherUsersHidden ? 1 : 0);
    const std::size_t count = std::min(info.events.size(), CRASH_LIST_MAX);
    writer.u32(static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i)
    {
        const CrashEvent& event = info.events[i];
        writer.u64(event.unixSeconds);
        writer.text(event.application);
        writer.u8(event.pid.has_value() ? 1 : 0);
        writer.u32(event.pid.value_or(0));
        writer.u8(event.uid.has_value() ? 1 : 0);
        writer.u32(event.uid.value_or(0));
        writer.u64(event.coreBytes);
        writer.u8(event.coreKept ? 1 : 0);
        writer.text(event.signal);
        writer.text(event.executable);
    }
    return MessageKind::CrashesResult;
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

/// An ActionResult's payload: the success flag, the error message, then the success note.
[[nodiscard]] inline ActionResult decodeActionResult(Reader& reader)
{
    const bool success = reader.flag();
    std::string errorMessage = reader.text();
    std::string successNote = reader.text();
    return ActionResult{.success = success, .errorMessage = std::move(errorMessage), .successNote = std::move(successNote)};
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
[[nodiscard]] inline OpenFilesReply decodeOpenFilesReply(Reader& reader)
{
    OpenFilesReply reply;
    const std::uint8_t value = reader.u8();
    if (value <= static_cast<std::uint8_t>(OpenFilesReadStatus::Failed))
    {
        reply.result.status = static_cast<OpenFilesReadStatus>(value);
    }
    else
    {
        reader.fail();
    }
    reply.result.detail = reader.text();
    reply.result.truncated = reader.flag();
    reply.result.namesIncomplete = reader.flag();
    reply.result.hexDescriptors = reader.flag();
    const std::uint32_t count = reader.u32();
    if (count > MAX_OPEN_FILES)
    {
        reader.fail();
    }
    for (std::uint32_t i = 0; reader.ok() && i < count; ++i)
    {
        reply.result.files.push_back(decodeOpenFile(reader));
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

/// A ProcessCountersResult's payload: the count (at most MAX_COUNTER_TARGETS), then the entries, each
/// with a flags byte refused past its three bits, and socket inodes refused without the sockets bit or past
/// MAX_COUNTER_SOCKET_INODES in all.
[[nodiscard]] inline ProcessCountersReply decodeProcessCountersReply(Reader& reader)
{
    ProcessCountersReply reply;
    const std::uint32_t count = reader.u32();
    if (count > MAX_COUNTER_TARGETS)
    {
        reader.fail();
        return reply;
    }
    reply.entries.reserve(count);
    std::size_t inodesLeft = MAX_COUNTER_SOCKET_INODES;
    for (std::uint32_t i = 0; reader.ok() && i < count; ++i)
    {
        ProcessCountersEntry entry;
        entry.target = reader.target();
        const std::uint8_t flags = reader.u8();
        if (flags > 7U)
        {
            reader.fail();
        }
        entry.handleCountRead = (flags & 1U) != 0;
        entry.ioRead = (flags & 2U) != 0;
        entry.socketsRead = (flags & 4U) != 0;
        entry.handleCount = reader.i32();
        entry.readBytes = reader.u64();
        entry.writeBytes = reader.u64();
        const std::uint32_t inodes = reader.u32();
        if (inodes > inodesLeft || (inodes != 0 && !entry.socketsRead))
        {
            reader.fail();
            break;
        }
        inodesLeft -= inodes;
        entry.socketInodes.reserve(inodes);
        for (std::uint32_t j = 0; reader.ok() && j < inodes; ++j)
        {
            entry.socketInodes.push_back(reader.u64());
        }
        reply.entries.push_back(std::move(entry));
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

/// A HiddenProcessesResult's payload: truncated, the count (refused past MAX_HIDDEN_PROCESSES), then each
/// process's target, its texts, and the command line's flag (0 or 1) before its text.
[[nodiscard]] inline HiddenProcessesReply decodeHiddenProcessesReply(Reader& reader)
{
    HiddenProcessesReply reply;
    reply.truncated = reader.flag();
    const std::uint32_t count = reader.u32();
    if (count > MAX_HIDDEN_PROCESSES)
    {
        reader.fail();
        return reply;
    }
    for (std::uint32_t i = 0; reader.ok() && i < count; ++i)
    {
        HiddenProcess process;
        process.target = reader.target();
        process.stat = reader.text();
        process.statm = reader.text();
        process.status = reader.text();
        const bool cmdlineRead = reader.flag();
        std::string cmdline = reader.text();
        if (cmdlineRead)
        {
            process.cmdline = std::move(cmdline);
        }
        else if (!cmdline.empty())
        {
            reader.fail(); // a command line it says it didn't read
        }
        process.io = reader.text();
        process.cgroup = reader.text();
        reply.processes.push_back(std::move(process));
    }
    return reply;
}

/// A MemoryRollupResult's payload: the tag (0 for figures, else 1 + the failure, refused past PermissionDenied),
/// then the three figures, which are always sent.
[[nodiscard]] inline MemoryRollupReply decodeMemoryRollupReply(Reader& reader)
{
    const std::uint8_t tag = reader.u8();
    MemoryRollup figures;
    figures.pssBytes = reader.u64();
    figures.ussBytes = reader.u64();
    figures.swapPssBytes = reader.u64();
    MemoryRollupReply reply;
    if (tag == 0)
    {
        reply.result = figures;
    }
    else if (tag - 1U <= static_cast<unsigned>(MemoryRollupFailure::PermissionDenied))
    {
        reply.result = std::unexpected(static_cast<MemoryRollupFailure>(tag - 1U));
    }
    else
    {
        reader.fail();
    }
    return reply;
}

/// A FirmwareFileResult's payload: the status (refused past Unreadable), then the bytes (capped at
/// MAX_FIRMWARE_FILE_BYTES).
[[nodiscard]] inline FirmwareFileReply decodeFirmwareFileReply(Reader& reader)
{
    const std::uint8_t status = reader.u8();
    FirmwareFileReply reply;
    if (status <= static_cast<std::uint8_t>(FirmwareFileStatus::Unreadable))
    {
        reply.status = static_cast<FirmwareFileStatus>(status);
    }
    else
    {
        reader.fail();
    }
    reply.bytes = reader.blob(MAX_FIRMWARE_FILE_BYTES);
    return reply;
}

/// One CrashesResult event: time, application, pid and uid (each always sent, kept only when flagged present),
/// core size, core kept, signal, then executable.
[[nodiscard]] inline CrashEvent decodeCrashEvent(Reader& reader)
{
    CrashEvent event;
    event.unixSeconds = reader.u64();
    event.application = reader.text();
    const bool hasPid = reader.flag();
    const std::uint32_t pid = reader.u32();
    if (hasPid)
    {
        event.pid = pid;
    }
    const bool hasUid = reader.flag();
    const std::uint32_t uid = reader.u32();
    if (hasUid)
    {
        event.uid = uid;
    }
    event.coreBytes = reader.u64();
    event.coreKept = reader.flag();
    event.signal = reader.text();
    event.executable = reader.text();
    return event;
}

/// A CrashesResult's payload: the availability flags and reasons (the OS family refused past Linux), a count
/// (refused past CRASH_LIST_MAX), then each event as decodeCrashEvent reads it.
[[nodiscard]] inline CrashesReply decodeCrashesReply(Reader& reader)
{
    CrashesReply reply;
    CrashesInfo& info = reply.info;
    info.available = reader.flag();
    const std::uint8_t family = reader.u8();
    if (family <= static_cast<std::uint8_t>(OsFamily::Linux))
    {
        info.family = static_cast<OsFamily>(family);
    }
    else
    {
        reader.fail();
    }
    info.listed = reader.flag();
    info.accessDenied = reader.flag();
    info.unavailableReason = reader.text();
    info.capped = reader.flag();
    info.journalRead = reader.flag();
    info.journalUnavailableReason = reader.text();
    info.otherUsersHidden = reader.flag();
    const std::uint32_t count = reader.u32();
    if (count > CRASH_LIST_MAX)
    {
        reader.fail();
    }
    for (std::uint32_t i = 0; reader.ok() && i < count; ++i)
    {
        info.events.push_back(decodeCrashEvent(reader));
    }
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
        return Detail::finish<Response>(reader, id, Detail::decodeOpenFilesReply(reader));
    case MessageKind::ConnectionsResult:
        return Detail::finish<Response>(reader, id, Detail::decodeConnectionsReply(reader));
    case MessageKind::MemoryRollupResult:
        return Detail::finish<Response>(reader, id, Detail::decodeMemoryRollupReply(reader));
    case MessageKind::FirmwareFileResult:
        return Detail::finish<Response>(reader, id, Detail::decodeFirmwareFileReply(reader));
    case MessageKind::CrashesResult:
        return Detail::finish<Response>(reader, id, Detail::decodeCrashesReply(reader));
    case MessageKind::ProcessCountersResult:
        return Detail::finish<Response>(reader, id, Detail::decodeProcessCountersReply(reader));
    case MessageKind::HiddenProcessesResult:
        return Detail::finish<Response>(reader, id, Detail::decodeHiddenProcessesReply(reader));
    case MessageKind::ServiceActionResult:
        return Detail::finish<Response>(reader, id, Detail::decodeServiceActionReply(reader));
    case MessageKind::Hello:
    case MessageKind::Signal:
    case MessageKind::SetPriority:
    case MessageKind::SetIoPriority:
    case MessageKind::SetAffinity:
    case MessageKind::ReadEnvironment:
    case MessageKind::ReadModules:
    case MessageKind::ReadOpenFiles:
    case MessageKind::ReadConnections:
    case MessageKind::ReadMemoryRollup:
    case MessageKind::ReadFirmwareFile:
    case MessageKind::ReadCrashes:
    case MessageKind::ReadProcessCounters:
    case MessageKind::ListHiddenProcesses:
    case MessageKind::ServiceAction:
        break;
    }
    return std::unexpected("Not a response");
}

} // namespace Platform::Broker
