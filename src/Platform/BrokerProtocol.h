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
// process and a reused PID can't redirect a request. This slice has the handshake and the process
// actions (#2308); the reads (#2309) and System Information (#2310) add their kinds in their phases.

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Platform::Broker
{

inline constexpr std::uint16_t PROTOCOL_VERSION = 1;
/// The largest frame either side accepts, header included.
inline constexpr std::size_t MAX_MESSAGE_BYTES = std::size_t{8} << 20U;
/// The longest string field (an error message, a note).
inline constexpr std::size_t MAX_STRING_BYTES = std::size_t{64} << 10U;
/// The most environment variables an EnvironmentReply carries; a read with more keeps the first.
inline constexpr std::size_t MAX_ENVIRONMENT_VARIABLES = std::size_t{1} << 16U;
/// The most modules a ModulesReply carries; a read with more keeps the first.
inline constexpr std::size_t MAX_MODULES = std::size_t{1} << 16U;
/// The most sockets a ConnectionsReply carries; a read with more keeps the first.
inline constexpr std::size_t MAX_CONNECTIONS = std::size_t{1} << 16U;
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
    ReadEnvironment = 32,    ///< Another user's process's environment, for Process Details (#2309)
    ReadModules = 33,        ///< Its loaded modules (#2309)
    ReadOpenFiles = 34,      ///< Its open files (#2309)
    ReadConnections = 35,    ///< Its TCP and UDP sockets (#2309)
    ReadMemoryRollup = 36,   ///< Its PSS and USS (#2309)
    ActionResult = 48,       ///< The answer to a process action
    EnvironmentResult = 49,  ///< The answer to ReadEnvironment
    ModulesResult = 50,      ///< The answer to ReadModules
    OpenFilesResult = 51,    ///< The answer to ReadOpenFiles
    ConnectionsResult = 52,  ///< The answer to ReadConnections
    MemoryRollupResult = 53, ///< The answer to ReadMemoryRollup
    Error = 63,              ///< A request the helper refused to read (malformed, or a kind it doesn't do)
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

/// Read a process's environment (#2309): the helper reads it as root, after the same identity check.
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
        if (left.result.status != right.result.status || left.result.variables.size() != right.result.variables.size())
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

/// List a process's loaded modules (#2309), as the helper reads them as root.
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
               std::ranges::equal(left.result.modules, right.result.modules, same);
    }
};

/// List a process's open files (#2309), as the helper reads them as root.
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

/// List a process's TCP and UDP sockets (#2309), as the helper reads them as root.
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

/// Read a process's PSS and USS (#2309), as the helper reads its smaps_rollup as root.
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
                             MemoryRollupRequest>;
using Response =
    std::variant<HelloReply, ActionResult, ErrorReply, EnvironmentReply, ModulesReply, OpenFilesReply, ConnectionsReply, MemoryRollupReply>;

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

/// The frame of request @p requestId asking for @p request.
[[nodiscard]] inline std::vector<std::uint8_t> encodeRequest(std::uint32_t requestId, const Request& request)
{
    Detail::Writer writer;
    MessageKind kind = MessageKind::Hello;
    if (const auto* hello = std::get_if<Hello>(&request))
    {
        writer.u16(hello->version);
    }
    else if (const auto* signal = std::get_if<SignalRequest>(&request))
    {
        kind = MessageKind::Signal;
        writer.target(signal->target);
        writer.u8(static_cast<std::uint8_t>(signal->signal));
    }
    else if (const auto* priority = std::get_if<PriorityRequest>(&request))
    {
        kind = MessageKind::SetPriority;
        writer.target(priority->target);
        writer.i32(priority->nice);
    }
    else if (const auto* io = std::get_if<IoPriorityRequest>(&request))
    {
        kind = MessageKind::SetIoPriority;
        writer.target(io->target);
        writer.u8(static_cast<std::uint8_t>(io->ioClass));
        writer.i32(io->level);
    }
    else if (const auto* environment = std::get_if<EnvironmentRequest>(&request))
    {
        kind = MessageKind::ReadEnvironment;
        writer.target(environment->target);
    }
    else if (const auto* modules = std::get_if<ModulesRequest>(&request))
    {
        kind = MessageKind::ReadModules;
        writer.target(modules->target);
    }
    else if (const auto* files = std::get_if<OpenFilesRequest>(&request))
    {
        kind = MessageKind::ReadOpenFiles;
        writer.target(files->target);
    }
    else if (const auto* connections = std::get_if<ConnectionsRequest>(&request))
    {
        kind = MessageKind::ReadConnections;
        writer.target(connections->target);
    }
    else if (const auto* rollup = std::get_if<MemoryRollupRequest>(&request))
    {
        kind = MessageKind::ReadMemoryRollup;
        writer.target(rollup->target);
    }
    else if (const auto* affinity = std::get_if<AffinityRequest>(&request))
    {
        kind = MessageKind::SetAffinity;
        writer.target(affinity->target);
        const std::span<const std::uint64_t> words = affinity->cpus.words();
        writer.u32(static_cast<std::uint32_t>(words.size()));
        for (const std::uint64_t word : words)
        {
            writer.u64(word);
        }
    }
    return writer.frame(kind, requestId);
}

/// The request in @p bytes (one whole frame), or why it was refused.
[[nodiscard]] inline std::expected<Decoded<Request>, std::string> decodeRequest(std::span<const std::uint8_t> bytes)
{
    const auto frame = Detail::readFrame(bytes);
    if (!frame.has_value())
    {
        return std::unexpected(frame.error());
    }
    Detail::Reader reader(frame->payload);
    switch (frame->kind)
    {
    case MessageKind::Hello:
        return Detail::finish<Request>(reader, frame->requestId, Hello{.version = reader.u16()});
    case MessageKind::Signal:
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
        return Detail::finish<Request>(reader, frame->requestId, SignalRequest{.target = target, .signal = signal});
    }
    case MessageKind::SetPriority:
    {
        const ProcessTarget target = reader.target();
        return Detail::finish<Request>(reader, frame->requestId, PriorityRequest{.target = target, .nice = reader.i32()});
    }
    case MessageKind::SetIoPriority:
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
        return Detail::finish<Request>(reader, frame->requestId, IoPriorityRequest{.target = target, .ioClass = ioClass, .level = level});
    }
    case MessageKind::SetAffinity:
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
        return Detail::finish<Request>(reader, frame->requestId, AffinityRequest{.target = target, .cpus = std::move(cpus)});
    }
    case MessageKind::ReadEnvironment:
    {
        const ProcessTarget target = reader.target();
        return Detail::finish<Request>(reader, frame->requestId, EnvironmentRequest{.target = target});
    }
    case MessageKind::ReadModules:
    {
        const ProcessTarget target = reader.target();
        return Detail::finish<Request>(reader, frame->requestId, ModulesRequest{.target = target});
    }
    case MessageKind::ReadOpenFiles:
    {
        const ProcessTarget target = reader.target();
        return Detail::finish<Request>(reader, frame->requestId, OpenFilesRequest{.target = target});
    }
    case MessageKind::ReadConnections:
    {
        const ProcessTarget target = reader.target();
        return Detail::finish<Request>(reader, frame->requestId, ConnectionsRequest{.target = target});
    }
    case MessageKind::ReadMemoryRollup:
    {
        const ProcessTarget target = reader.target();
        return Detail::finish<Request>(reader, frame->requestId, MemoryRollupRequest{.target = target});
    }
    case MessageKind::HelloReply:
    case MessageKind::ActionResult:
    case MessageKind::EnvironmentResult:
    case MessageKind::ModulesResult:
    case MessageKind::OpenFilesResult:
    case MessageKind::ConnectionsResult:
    case MessageKind::MemoryRollupResult:
    case MessageKind::Error:
        break;
    }
    return std::unexpected("Not a request this helper does");
}

/// The frame of the response to request @p requestId.
[[nodiscard]] inline std::vector<std::uint8_t> encodeResponse(std::uint32_t requestId, const Response& response)
{
    Detail::Writer writer;
    MessageKind kind = MessageKind::HelloReply;
    if (const auto* hello = std::get_if<HelloReply>(&response))
    {
        writer.u16(hello->version);
        writer.u8(hello->accepted ? 1 : 0);
    }
    else if (const auto* result = std::get_if<ActionResult>(&response))
    {
        kind = MessageKind::ActionResult;
        writer.u8(result->success ? 1 : 0);
        writer.text(result->errorMessage);
        writer.text(result->successNote);
    }
    else if (const auto* error = std::get_if<ErrorReply>(&response))
    {
        kind = MessageKind::Error;
        writer.text(error->message);
    }
    else if (const auto* environment = std::get_if<EnvironmentReply>(&response))
    {
        kind = MessageKind::EnvironmentResult;
        writer.u8(static_cast<std::uint8_t>(environment->result.status));
        const std::size_t count = std::min(environment->result.variables.size(), MAX_ENVIRONMENT_VARIABLES);
        writer.u32(static_cast<std::uint32_t>(count));
        for (std::size_t i = 0; i < count; ++i)
        {
            writer.text(environment->result.variables[i].name);
            writer.text(environment->result.variables[i].value);
        }
    }
    else if (const auto* modules = std::get_if<ModulesReply>(&response))
    {
        kind = MessageKind::ModulesResult;
        writer.u8(static_cast<std::uint8_t>(modules->result.status));
        writer.text(modules->result.detail);
        const std::size_t count = std::min(modules->result.modules.size(), MAX_MODULES);
        writer.u32(static_cast<std::uint32_t>(count));
        for (std::size_t i = 0; i < count; ++i)
        {
            const ProcessModule& module = modules->result.modules[i];
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
    }
    else if (const auto* files = std::get_if<OpenFilesReply>(&response))
    {
        kind = MessageKind::OpenFilesResult;
        // As many files as fit in one frame: thousands of long paths could pass MAX_MESSAGE_BYTES, so the
        // list stops short of it and says it was cut, as a read past MAX_OPEN_FILES does.
        const std::size_t limit = std::min(files->result.files.size(), MAX_OPEN_FILES);
        std::size_t count = 0;
        std::size_t bytes = 0;
        constexpr std::size_t FIXED_BYTES = 8 + 1 + 4 + 1 + 4 + 1; // descriptor, kind, path size, flag, flags, deleted
        constexpr std::size_t ROOM = MAX_MESSAGE_BYTES - (4 + HEADER_BYTES) - (1 + 4 + MAX_STRING_BYTES + 3 + 4);
        while (count < limit)
        {
            const std::size_t entry = FIXED_BYTES + std::min(files->result.files[count].path.size(), MAX_STRING_BYTES);
            if (bytes + entry > ROOM)
            {
                break;
            }
            bytes += entry;
            ++count;
        }
        writer.u8(static_cast<std::uint8_t>(files->result.status));
        writer.text(files->result.detail);
        writer.u8((files->result.truncated || count < files->result.files.size()) ? 1 : 0);
        writer.u8(files->result.namesIncomplete ? 1 : 0);
        writer.u8(files->result.hexDescriptors ? 1 : 0);
        writer.u32(static_cast<std::uint32_t>(count));
        for (std::size_t i = 0; i < count; ++i)
        {
            const OpenFile& file = files->result.files[i];
            writer.u64(file.descriptor);
            writer.u8(static_cast<std::uint8_t>(file.kind));
            writer.text(file.path);
            writer.u8(file.flags.has_value() ? 1 : 0);
            writer.u32(file.flags.value_or(0));
            writer.u8(file.deleted ? 1 : 0);
        }
    }
    else if (const auto* connections = std::get_if<ConnectionsReply>(&response))
    {
        kind = MessageKind::ConnectionsResult;
        writer.u8(static_cast<std::uint8_t>(connections->result.status));
        writer.text(connections->result.detail);
        const std::size_t count = std::min(connections->result.connections.size(), MAX_CONNECTIONS);
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
            const ProcessConnection& connection = connections->result.connections[i];
            writer.u8(static_cast<std::uint8_t>(connection.protocol));
            writer.u8(static_cast<std::uint8_t>(connection.family));
            endpoint(connection.local);
            endpoint(connection.remote);
            writer.u8(static_cast<std::uint8_t>(connection.state));
        }
    }
    else if (const auto* rollup = std::get_if<MemoryRollupReply>(&response))
    {
        kind = MessageKind::MemoryRollupResult;
        // 0: the figures follow; else 1 + the failure.
        writer.u8(rollup->result.has_value() ? 0 : static_cast<std::uint8_t>(1 + static_cast<std::uint8_t>(rollup->result.error())));
        const MemoryRollup figures = rollup->result.value_or(MemoryRollup{});
        writer.u64(figures.pssBytes);
        writer.u64(figures.ussBytes);
        writer.u64(figures.swapPssBytes);
    }
    return writer.frame(kind, requestId);
}

/// The response in @p bytes (one whole frame), or why it couldn't be read.
[[nodiscard]] inline std::expected<Decoded<Response>, std::string> decodeResponse(std::span<const std::uint8_t> bytes)
{
    const auto frame = Detail::readFrame(bytes);
    if (!frame.has_value())
    {
        return std::unexpected(frame.error());
    }
    Detail::Reader reader(frame->payload);
    switch (frame->kind)
    {
    case MessageKind::HelloReply:
    {
        const std::uint16_t version = reader.u16();
        return Detail::finish<Response>(reader, frame->requestId, HelloReply{.version = version, .accepted = reader.flag()});
    }
    case MessageKind::ActionResult:
    {
        const bool success = reader.flag();
        std::string errorMessage = reader.text();
        std::string successNote = reader.text();
        return Detail::finish<Response>(
            reader,
            frame->requestId,
            ActionResult{.success = success, .errorMessage = std::move(errorMessage), .successNote = std::move(successNote)});
    }
    case MessageKind::Error:
        return Detail::finish<Response>(reader, frame->requestId, ErrorReply{.message = reader.text()});
    case MessageKind::EnvironmentResult:
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
        return Detail::finish<Response>(reader, frame->requestId, std::move(reply));
    }
    case MessageKind::ModulesResult:
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
        return Detail::finish<Response>(reader, frame->requestId, std::move(reply));
    }
    case MessageKind::OpenFilesResult:
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
            reply.result.files.push_back(std::move(file));
        }
        return Detail::finish<Response>(reader, frame->requestId, std::move(reply));
    }
    case MessageKind::ConnectionsResult:
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
        return Detail::finish<Response>(reader, frame->requestId, std::move(reply));
    }
    case MessageKind::MemoryRollupResult:
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
        return Detail::finish<Response>(reader, frame->requestId, std::move(reply));
    }
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
        break;
    }
    return std::unexpected("Not a response");
}

} // namespace Platform::Broker
