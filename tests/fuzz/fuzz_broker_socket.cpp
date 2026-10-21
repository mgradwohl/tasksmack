#include "Platform/BrokerProtocol.h"
#include "Platform/Linux/BrokerSocket.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

// The privileged helper's frame reassembly (#2643): BrokerSocket::receiveFrame() is the first code in the
// root helper to touch what an unprivileged client sends, joining a frame's 64 KiB SEQPACKET packets by its
// length prefix. The input describes the packets a client sends over a real socketpair, then closes; the
// frame that comes back must be the first packets exactly, never past the frame's length or
// MAX_MESSAGE_BYTES, and a packet over MAX_PACKET_BYTES or a frame cut short must be refused.
//
// Input: a run of packet records, each a selector byte, then (for selectors 2 and 3) a 16-bit
// little-endian length, then a count byte and that many bytes, which start the packet (the rest is zeros).
// Selector & 3 picks the packet's size: 0 a full MAX_PACKET_BYTES packet, 1 one byte over it, 2 and 3 the
// given length.

// NOLINTBEGIN(misc-include-cleaner) - POSIX socket headers: include-cleaner lacks mappings for AF_UNIX, SOCK_SEQPACKET, MSG_NOSIGNAL
namespace
{

namespace Broker = Platform::Broker;
namespace BrokerSocket = Platform::BrokerSocket;

// Enough full packets for the largest frame, and two more to run past it.
constexpr std::size_t MAX_PACKETS = (Broker::MAX_MESSAGE_BYTES / BrokerSocket::MAX_PACKET_BYTES) + 2;

[[nodiscard]] std::vector<std::vector<std::uint8_t>> packetsFrom(std::span<const std::uint8_t> input)
{
    std::vector<std::vector<std::uint8_t>> packets;
    std::size_t at = 0;
    const auto next = [&input, &at]() -> std::uint8_t
    {
        return at < input.size() ? input[at++] : 0;
    };
    while (at < input.size() && packets.size() < MAX_PACKETS)
    {
        const std::uint8_t selector = next();
        std::size_t size = 0;
        switch (selector & 3U)
        {
        case 0:
            size = BrokerSocket::MAX_PACKET_BYTES;
            break;
        case 1:
            size = BrokerSocket::MAX_PACKET_BYTES + 1;
            break;
        default:
            size = next();
            size |= static_cast<std::size_t>(next()) << 8U;
            break;
        }
        std::vector<std::uint8_t> packet(size, 0);
        const std::size_t given = std::min({static_cast<std::size_t>(next()), size, input.size() - at});
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(at), given, packet.begin());
        at += given;
        packets.push_back(std::move(packet));
    }
    return packets;
}

/// Whether @p frame is packets[0..k) joined, for some k >= 1; @p joined is set to that k.
[[nodiscard]] bool
isLeadingPackets(std::span<const std::uint8_t> frame, const std::vector<std::vector<std::uint8_t>>& packets, std::size_t& joined)
{
    std::size_t at = 0;
    joined = 0;
    for (const std::vector<std::uint8_t>& packet : packets)
    {
        if (at == frame.size() && joined > 0)
        {
            return true;
        }
        if (packet.size() > frame.size() - at || !std::equal(packet.begin(), packet.end(), frame.begin() + static_cast<std::ptrdiff_t>(at)))
        {
            return false;
        }
        at += packet.size();
        ++joined;
    }
    return at == frame.size() && joined > 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    const std::vector<std::vector<std::uint8_t>> packets = packetsFrom(std::span<const std::uint8_t>(data, size));

    std::array<int, 2> fds{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds.data()) != 0)
    {
        return 0;
    }
    // The client: sends every packet, then closes. A send fails once the helper has stopped reading and
    // closed its end, which is how the thread ends early.
    std::thread client(
        [&packets, fd = fds[1]]
        {
            for (const std::vector<std::uint8_t>& packet : packets)
            {
                if (::send(fd, packet.data(), packet.size(), MSG_NOSIGNAL) < 0)
                {
                    break;
                }
            }
            ::close(fd);
        });
    // The client always closes, so the frame ends one way or another long before this.
    constexpr std::chrono::seconds BACKSTOP{30};
    const auto frame = BrokerSocket::receiveFrame(fds[0], BACKSTOP, std::stop_token{});
    ::close(fds[0]);
    client.join();

    if (!frame.has_value())
    {
        switch (frame.error())
        {
        case BrokerSocket::ReceiveEnd::Closed:
            // Only before the first byte: no packet, or an empty one, which reads as the end of the stream.
            if (!packets.empty() && !packets.front().empty())
            {
                std::abort();
            }
            break;
        case BrokerSocket::ReceiveEnd::Failed:
            break;
        case BrokerSocket::ReceiveEnd::TimedOut:
        case BrokerSocket::ReceiveEnd::Stopped:
            std::abort(); // a hang, or a stop nobody asked for
        }
        return 0;
    }
    std::size_t joined = 0;
    if (!isLeadingPackets(*frame, packets, joined) || packets.front().size() > BrokerSocket::MAX_PACKET_BYTES)
    {
        std::abort();
    }
    if (joined > 1)
    {
        // Only a full first packet starts a longer frame, and the frame never runs past its length prefix.
        const std::vector<std::uint8_t>& first = packets.front();
        std::uint32_t length = 0;
        std::memcpy(&length, first.data(), sizeof(length)); // little-endian, like every host this runs on
        if (first.size() != BrokerSocket::MAX_PACKET_BYTES || frame->size() > std::size_t{length} + 4 ||
            frame->size() > Broker::MAX_MESSAGE_BYTES)
        {
            std::abort();
        }
        // Short of its length only when its last packet was short: one the client cut off is refused.
        if (frame->size() != std::size_t{length} + 4 && packets[joined - 1].size() == BrokerSocket::MAX_PACKET_BYTES)
        {
            std::abort();
        }
    }
    return 0;
}
// NOLINTEND(misc-include-cleaner)
