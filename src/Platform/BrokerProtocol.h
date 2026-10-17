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
    ActionResult = 48, ///< The answer to a process action
    Error = 63,        ///< A request the helper refused to read (malformed, or a kind it doesn't do)
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

using Request = std::variant<Hello, SignalRequest, PriorityRequest, IoPriorityRequest, AffinityRequest>;
using Response = std::variant<HelloReply, ActionResult, ErrorReply>;

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
    case MessageKind::HelloReply:
    case MessageKind::ActionResult:
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
    case MessageKind::Hello:
    case MessageKind::Signal:
    case MessageKind::SetPriority:
    case MessageKind::SetIoPriority:
    case MessageKind::SetAffinity:
        break;
    }
    return std::unexpected("Not a response");
}

} // namespace Platform::Broker
