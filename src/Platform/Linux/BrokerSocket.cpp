#include "BrokerSocket.h"

#include "Platform/BrokerProtocol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
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
    while (true)
    {
        // MSG_NOSIGNAL: a peer that has gone is an error here, not a SIGPIPE that ends the process.
        const ssize_t sent = ::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
        if (std::cmp_equal(sent, frame.size()))
        {
            return true;
        }
        if (sent < 0 && errno == EINTR)
        {
            continue;
        }
        error = sent < 0 ? std::string(std::strerror(errno))
                         : std::string("a short send"); // NOLINT(concurrency-mt-unsafe) - one thread per socket
        return false;
    }
}

std::expected<std::vector<std::uint8_t>, ReceiveEnd>
receiveFrame(int fd, std::chrono::milliseconds timeout, const std::stop_token& stopToken)
{
    constexpr std::chrono::milliseconds POLL_STEP{100};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
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
        // One packet is one frame. Its real size first (MSG_PEEK | MSG_TRUNC reports it without taking
        // the packet), so the buffer is exactly that and a packet larger than the protocol allows is
        // refused before anything is allocated for it.
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
        if (size < 0 || std::cmp_greater(size, Broker::MAX_MESSAGE_BYTES))
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

} // namespace Platform::BrokerSocket
// NOLINTEND(misc-include-cleaner)
