#pragma once

// The pure parts of the Windows UDP event-trace collector (#1492, WindowsEtwNetCollector): decoding a
// Microsoft-Windows-Kernel-Network UDP event's payload, summing it per process, turning the sums
// into socket-traffic samples, and the session name that lets a later TaskSmack find and stop a
// session a crashed one left behind. No Windows header, so the tests build and run on every platform
// (tests/Platform/WindowsMath/test_WindowsEtwNetMath.cpp).

#include "Platform/IUdpTrafficCollector.h"
#include "Platform/ProcessTypes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Platform::EtwNet
{

// Microsoft-Windows-Kernel-Network {7DD42A49-5329-4832-8DFD-43D979153A88}: its UDP events, from the
// provider's manifest (Get-WinEvent -ListProvider Microsoft-Windows-Kernel-Network). All are version 0.
inline constexpr std::uint16_t UDP4_SEND_EVENT_ID = 42; // "UDPv4: %2 bytes transmitted ..."
inline constexpr std::uint16_t UDP4_RECV_EVENT_ID = 43; // "UDPv4: %2 bytes received ..."
inline constexpr std::uint16_t UDP6_SEND_EVENT_ID = 58; // "UDPv6: %2 bytes transmitted ..."
inline constexpr std::uint16_t UDP6_RECV_EVENT_ID = 59; // "UDPv6: %2 bytes received ..."

// The provider's keywords: one per address family. Each also covers its TCP events, so the session
// also filters by event ID (EVENT_FILTER_TYPE_EVENT_ID), and the callback checks the ID again: TCP is
// counted by EStats, and counting it here too would count it twice.
inline constexpr std::uint64_t KEYWORD_IPV4 = 0x10;
inline constexpr std::uint64_t KEYWORD_IPV6 = 0x20;

// The version 0 payloads (the manifest's templates): PID (UInt32), size (UInt32), daddr, saddr,
// dport (UInt16), sport (UInt16), seqnum (UInt32), connid (UInt32). An IPv4 address is a UInt32, an
// IPv6 one 16 bytes. PID and size lead both, so they sit at fixed offsets.
inline constexpr std::size_t PID_OFFSET = 0;
inline constexpr std::size_t SIZE_OFFSET = 4;
inline constexpr std::size_t UDP4_PAYLOAD_SIZE = 4 + 4 + 4 + 4 + 2 + 2 + 4 + 4;
inline constexpr std::size_t UDP6_PAYLOAD_SIZE = 4 + 4 + 16 + 16 + 2 + 2 + 4 + 4;

/// Which way an event's bytes went.
enum class UdpDirection : std::uint8_t
{
    Sent,
    Received,
};

/// One UDP datagram as an event reports it.
struct UdpPacket
{
    std::uint32_t pid = 0;
    std::uint32_t bytes = 0;
    UdpDirection direction = UdpDirection::Sent;
};

/// The direction of a UDP event, or nullopt for any other event of the provider (TCP, connection
/// events), which the collector ignores.
[[nodiscard]] constexpr std::optional<UdpDirection> udpEventDirection(std::uint16_t eventId) noexcept
{
    switch (eventId)
    {
    case UDP4_SEND_EVENT_ID:
    case UDP6_SEND_EVENT_ID:
        return UdpDirection::Sent;
    case UDP4_RECV_EVENT_ID:
    case UDP6_RECV_EVENT_ID:
        return UdpDirection::Received;
    default:
        return std::nullopt;
    }
}

/// The smallest payload a UDP event of @p eventId can have: its version 0 template. 0 for other IDs.
[[nodiscard]] constexpr std::size_t udpPayloadSize(std::uint16_t eventId) noexcept
{
    switch (eventId)
    {
    case UDP4_SEND_EVENT_ID:
    case UDP4_RECV_EVENT_ID:
        return UDP4_PAYLOAD_SIZE;
    case UDP6_SEND_EVENT_ID:
    case UDP6_RECV_EVENT_ID:
        return UDP6_PAYLOAD_SIZE;
    default:
        return 0;
    }
}

/// Decode one event's payload (EVENT_RECORD::UserData, UserDataLength bytes) at the fixed offsets of
/// its version 0 template. A later version is read the same way: manifest templates only grow at the
/// end, so PID and size stay first. Nullopt for an event that isn't UDP, a payload shorter than the
/// template (a malformed or truncated event), and PID 0 (the idle process: not attributable).
/// The payload is little-endian, as is every machine Windows runs on.
[[nodiscard]] inline std::optional<UdpPacket> parseUdpEvent(std::uint16_t eventId, std::span<const std::byte> payload) noexcept
{
    const std::optional<UdpDirection> direction = udpEventDirection(eventId);
    if (!direction.has_value() || payload.size() < udpPayloadSize(eventId))
    {
        return std::nullopt;
    }
    UdpPacket packet{.direction = *direction};
    std::memcpy(&packet.pid, payload.subspan(PID_OFFSET, sizeof(packet.pid)).data(), sizeof(packet.pid));
    std::memcpy(&packet.bytes, payload.subspan(SIZE_OFFSET, sizeof(packet.bytes)).data(), sizeof(packet.bytes));
    if (packet.pid == 0)
    {
        return std::nullopt;
    }
    return packet;
}

/// Samples in a row with no new bytes after which a process's counters are dropped: it has exited
/// or gone quiet. Dropped, it leaves the reading, and if it sends again it starts from 0 as a new
/// "connection", whose bytes Domain credits in full (they all came after the reading it was dropped
/// from). Keeps the map to the processes using UDP now rather than every PID seen since start.
inline constexpr std::uint32_t IDLE_SAMPLES_BEFORE_FORGET = 120;

/// The per-process UDP byte sums the event callback adds to, and the sampler reads. A process's
/// first event inserts its entry (one allocation); every later event only adds to it. Not
/// thread-safe: the collector holds it under a mutex.
class UdpByteCounters
{
  public:
    void add(const UdpPacket& packet)
    {
        Entry& entry = m_Entries[packet.pid];
        if (packet.direction == UdpDirection::Sent)
        {
            entry.bytesSent += packet.bytes;
        }
        else
        {
            entry.bytesReceived += packet.bytes;
        }
        entry.grew = true;
    }

    /// Every process's cumulative bytes, for one socket-traffic reading. A process with no new bytes
    /// for @p idleSamplesBeforeForget samples in a row is dropped instead (see
    /// IDLE_SAMPLES_BEFORE_FORGET).
    [[nodiscard]] std::vector<UdpProcessTraffic> sample(std::uint32_t idleSamplesBeforeForget = IDLE_SAMPLES_BEFORE_FORGET)
    {
        std::vector<UdpProcessTraffic> totals;
        totals.reserve(m_Entries.size());
        auto it = m_Entries.begin();
        while (it != m_Entries.end())
        {
            Entry& entry = it->second;
            entry.idleSamples = entry.grew ? 0 : entry.idleSamples + 1;
            entry.grew = false;
            if (entry.idleSamples >= idleSamplesBeforeForget)
            {
                it = m_Entries.erase(it);
                continue;
            }
            totals.push_back(UdpProcessTraffic{.pid = it->first, .bytesSent = entry.bytesSent, .bytesReceived = entry.bytesReceived});
            ++it;
        }
        return totals;
    }

    void clear() noexcept
    {
        m_Entries.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Entries.size();
    }

  private:
    struct Entry
    {
        std::uint64_t bytesSent = 0;
        std::uint64_t bytesReceived = 0;
        std::uint32_t idleSamples = 0; // Samples in a row with no new bytes
        bool grew = false;             // New bytes since the last sample
    };
    std::unordered_map<std::uint32_t, Entry> m_Entries;
};

/// The socket key of a process's UDP counters. Bit 63 set: the TCP keys (estatsConnectionKey())
/// always have it clear, so the two never collide.
[[nodiscard]] constexpr std::uint64_t udpSocketKey(std::uint32_t pid) noexcept
{
    return (std::uint64_t{1} << 63U) | pid;
}

/// Add each process's UDP counters to a socket-traffic reading as one synthetic, cumulative
/// "connection" (key udpSocketKey()). Its owner is matched by PID alone (start time 0), as the TCP
/// rows are: the events report only the PID.
inline void appendUdpSamples(std::vector<SocketTrafficSample>& sockets, std::span<const UdpProcessTraffic> udp)
{
    sockets.reserve(sockets.size() + udp.size());
    for (const UdpProcessTraffic& process : udp)
    {
        sockets.push_back(SocketTrafficSample{.key = udpSocketKey(process.pid),
                                              .pid = static_cast<std::int32_t>(process.pid),
                                              .bytesReceived = process.bytesReceived,
                                              .bytesSent = process.bytesSent});
    }
}

// ==========================================================================
// Session names and stale sessions
// ==========================================================================

/// Every session TaskSmack starts is named this, then "<pid>-<creation time>": the process that owns
/// it, and that process's creation time (FILETIME ticks), so a PID reused by another process isn't
/// taken for the owner.
inline constexpr std::string_view SESSION_NAME_PREFIX = "TaskSmack-UDP-";

/// The session's owner, from its name.
struct SessionOwner
{
    std::uint32_t pid = 0;
    std::uint64_t creationTime = 0;

    bool operator==(const SessionOwner&) const = default;
};

[[nodiscard]] inline std::string sessionName(const SessionOwner& owner)
{
    std::string name(SESSION_NAME_PREFIX);
    name += std::to_string(owner.pid);
    name += '-';
    name += std::to_string(owner.creationTime);
    return name;
}

namespace Detail
{
/// Parse a run of decimal digits that fits in @p max. Nullopt for an empty run, a non-digit or overflow.
[[nodiscard]] constexpr std::optional<std::uint64_t> parseDecimal(std::string_view text, std::uint64_t max) noexcept
{
    if (text.empty())
    {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    std::size_t index = 0;
    while (index < text.size())
    {
        const char c = text[index];
        if (c < '0' || c > '9')
        {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (max - digit) / 10)
        {
            return std::nullopt;
        }
        value = (value * 10) + digit;
        ++index;
    }
    return value;
}
} // namespace Detail

/// The owner a session name records, or nullopt when it isn't one of TaskSmack's (another tool's
/// session, or a name that doesn't parse: never touched).
[[nodiscard]] constexpr std::optional<SessionOwner> parseSessionName(std::string_view name) noexcept
{
    if (!name.starts_with(SESSION_NAME_PREFIX))
    {
        return std::nullopt;
    }
    name.remove_prefix(SESSION_NAME_PREFIX.size());
    const std::size_t dash = name.find('-');
    if (dash == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> pid = Detail::parseDecimal(name.substr(0, dash), std::numeric_limits<std::uint32_t>::max());
    const std::optional<std::uint64_t> creation = Detail::parseDecimal(name.substr(dash + 1), std::numeric_limits<std::uint64_t>::max());
    if (!pid.has_value() || !creation.has_value())
    {
        return std::nullopt;
    }
    return SessionOwner{.pid = static_cast<std::uint32_t>(*pid), .creationTime = *creation};
}

/// What the collector found out about a session owner's process.
enum class OwnerLiveness : std::uint8_t
{
    Gone,      ///< No such process, or it has exited
    Alive,     ///< Running, with the creation time the name records: the owner itself
    PidReused, ///< A process with that PID runs, but it was created at another time
    Unknown,   ///< It couldn't be checked (the process can't be opened, a call failed)
};

/// Whether to stop a TaskSmack session found at start: one left by a TaskSmack that crashed (its
/// owner is gone, or its PID now belongs to another process) or by this one, but never one a
/// TaskSmack still running owns, nor one whose owner couldn't be checked.
[[nodiscard]] constexpr bool shouldStopSession(const SessionOwner& owner, const SessionOwner& self, OwnerLiveness liveness) noexcept
{
    if (owner == self)
    {
        return true; // This process's own (a stop that failed earlier): replaced by the new one
    }
    return liveness == OwnerLiveness::Gone || liveness == OwnerLiveness::PidReused;
}

} // namespace Platform::EtwNet
