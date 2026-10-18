#pragma once

// The Linux transport of the privileged broker (#2307): BrokerProtocol frames over an AF_UNIX
// SOCK_SEQPACKET socket. A frame goes in packets of at most MAX_PACKET_BYTES, since a packet can't be
// larger than the socket's send buffer (#2367); the receiver joins them by the frame's length prefix.
// The client makes a socketpair and hands the helper one end as its stdin (pkexec keeps fds 0-2);
// there is no socket on the filesystem for anyone else to connect to. Each side checks the other's
// credentials (SO_PEERCRED) before it trusts a frame.

#include "Platform/BrokerProtocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <sys/types.h>

namespace Platform::BrokerSocket
{

/// The largest packet either side sends or accepts: well under the default AF_UNIX send buffer
/// (net.core.wmem_default, 212992 bytes, less 32 of overhead), so it never needs SO_SNDBUF raised.
inline constexpr std::size_t MAX_PACKET_BYTES = std::size_t{64} << 10U;

/// The peer of a connected AF_UNIX socket, as the kernel recorded it when the pair was made.
struct PeerCredentials
{
    pid_t pid = 0;
    uid_t uid = 0;
    gid_t gid = 0;
};

/// @p fd's peer (SO_PEERCRED), or nullopt when it can't be read (not a Unix socket).
[[nodiscard]] std::optional<PeerCredentials> peerCredentials(int fd) noexcept;

/// Whether @p fd is a connected AF_UNIX SOCK_SEQPACKET socket: the only kind the helper talks over.
[[nodiscard]] bool isSeqPacketSocket(int fd) noexcept;

/// Sends one frame, in as many packets as it takes. False (and why in @p error) when the peer has gone or
/// the send failed.
[[nodiscard]] bool sendFrame(int fd, std::span<const std::uint8_t> frame, std::string& error);

/// What receiveFrame() found when there was no frame.
enum class ReceiveEnd : std::uint8_t
{
    Closed,   ///< The peer closed its end: the normal end of a session
    TimedOut, ///< Nothing within the timeout
    Stopped,  ///< The stop token was stopped
    Failed,   ///< A read error, a packet or frame larger than allowed, or a frame cut short
};

/// Waits up to @p timeout for one whole frame (all its packets), polling @p stopToken every 100 ms. A frame
/// whose first packet came but not the rest is Failed, not TimedOut: the socket is mid-frame (#2406).
[[nodiscard]] std::expected<std::vector<std::uint8_t>, ReceiveEnd>
receiveFrame(int fd, std::chrono::milliseconds timeout, const std::stop_token& stopToken);

} // namespace Platform::BrokerSocket
