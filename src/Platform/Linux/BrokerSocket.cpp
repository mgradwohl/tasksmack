#include "BrokerSocket.h"

#include "Platform/BrokerProtocol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/poll.h>
#include <sys/socket.h>

// NOLINTBEGIN(misc-include-cleaner) - POSIX socket headers: include-cleaner lacks mappings for SOL_SOCKET, SO_*, ssize_t, pollfd, POLLIN
namespace Platform::BrokerSocket
{

std::optional<PeerCredentials> peerCredentials(int fd) noexcept
{
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 || length != sizeof(credentials))
    {
        return std::nullopt;
    }
    return PeerCredentials{.pid = credentials.pid, .uid = credentials.uid, .gid = credentials.gid};
}

bool isSeqPacketSocket(int fd) noexcept
{
    int type = 0;
    socklen_t typeLength = sizeof(type);
    int domain = 0;
    socklen_t domainLength = sizeof(domain);
    return ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &typeLength) == 0 && type == SOCK_SEQPACKET &&
           ::getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &domainLength) == 0 && domain == AF_UNIX;
}

bool sendFrame(int fd, std::span<const std::uint8_t> frame, std::string& error)
{
    if (frame.size() > Broker::MAX_MESSAGE_BYTES)
    {
        error = "a frame larger than the protocol allows";
        return false;
    }
    // In packets of at most MAX_PACKET_BYTES: a SEQPACKET packet can't exceed the socket's send buffer
    // (about 208 KiB by default), far less than a frame may be (#2367). The receiver reassembles them by
    // the frame's own length.
    std::size_t at = 0;
    while (at < frame.size())
    {
        const std::span<const std::uint8_t> packet = frame.subspan(at, std::min(MAX_PACKET_BYTES, frame.size() - at));
        // MSG_NOSIGNAL: a peer that has gone is an error here, not a SIGPIPE that ends the process.
        const ssize_t sent = ::send(fd, packet.data(), packet.size(), MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR)
        {
            continue;
        }
        if (!std::cmp_equal(sent, packet.size()))
        {
            error = sent < 0 ? std::system_category().message(errno) : std::string("a short send");
            return false;
        }
        at += packet.size();
    }
    return true;
}

namespace
{

/// The next packet, waiting until @p deadline and polling @p stopToken every 100 ms. Refuses one larger
/// than MAX_PACKET_BYTES before anything is allocated for it.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, ReceiveEnd>
receivePacket(int fd, std::chrono::steady_clock::time_point deadline, const std::stop_token& stopToken)
{
    constexpr std::chrono::milliseconds POLL_STEP{100};
    while (true)
    {
        if (stopToken.stop_requested())
        {
            return std::unexpected(ReceiveEnd::Stopped);
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0)
        {
            return std::unexpected(ReceiveEnd::TimedOut);
        }
        pollfd waiting{.fd = fd, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&waiting, 1, static_cast<int>(std::min(left, POLL_STEP).count()));
        if (ready < 0 && errno != EINTR)
        {
            return std::unexpected(ReceiveEnd::Failed);
        }
        if (ready <= 0)
        {
            continue;
        }
        // Its real size first (MSG_PEEK | MSG_TRUNC reports it without taking the packet), so the buffer
        // is exactly that.
        std::uint8_t probe = 0;
        const ssize_t size = ::recv(fd, &probe, 1, MSG_PEEK | MSG_TRUNC);
        if (size < 0 && errno == EINTR)
        {
            continue;
        }
        if (size == 0)
        {
            return std::unexpected(ReceiveEnd::Closed);
        }
        if (size < 0 || std::cmp_greater(size, MAX_PACKET_BYTES))
        {
            return std::unexpected(ReceiveEnd::Failed);
        }
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
        const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (received != size)
        {
            return std::unexpected(ReceiveEnd::Failed);
        }
        return buffer;
    }
}

} // namespace

std::expected<std::vector<std::uint8_t>, ReceiveEnd>
receiveFrame(int fd, std::chrono::milliseconds timeout, const std::stop_token& stopToken)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto frame = receivePacket(fd, deadline, stopToken);
    if (!frame.has_value())
    {
        return frame;
    }
    // A frame larger than one packet follows in full packets, its last one shorter or exactly what its
    // length prefix (the first four bytes, little-endian) still lacks (#2367). Anything else -- a short
    // or malformed frame -- is returned as it is, for the decoder to refuse, as before.
    const auto declaredTotal = [&frame] -> std::size_t
    {
        const std::vector<std::uint8_t>& bytes = *frame;
        const std::size_t length = static_cast<std::size_t>(bytes[0]) | (static_cast<std::size_t>(bytes[1]) << 8U) |
                                   (static_cast<std::size_t>(bytes[2]) << 16U) | (static_cast<std::size_t>(bytes[3]) << 24U);
        return length + 4;
    };
    if (frame->size() != MAX_PACKET_BYTES || declaredTotal() > Broker::MAX_MESSAGE_BYTES)
    {
        return frame;
    }
    const std::size_t total = declaredTotal();
    frame->reserve(total);
    while (frame->size() < total)
    {
        auto packet = receivePacket(fd, deadline, stopToken);
        if (!packet.has_value())
        {
            // A peer that closes mid-frame has sent something broken, not ended the session cleanly. One cut
            // short by the timeout is broken too: its remaining packets would be read as the next frame, by a
            // client that keeps the channel after a timeout (#2406).
            return std::unexpected(packet.error() == ReceiveEnd::Stopped ? ReceiveEnd::Stopped : ReceiveEnd::Failed);
        }
        if (packet->size() > total - frame->size())
        {
            return std::unexpected(ReceiveEnd::Failed);
        }
        const bool last = packet->size() < MAX_PACKET_BYTES;
        frame->insert(frame->end(), packet->begin(), packet->end());
        if (last)
        {
            break; // short of its length: the decoder refuses it
        }
    }
    return frame;
}

} // namespace Platform::BrokerSocket
// NOLINTEND(misc-include-cleaner)
