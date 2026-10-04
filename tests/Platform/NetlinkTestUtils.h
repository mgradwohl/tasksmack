#pragma once

/// @file NetlinkTestUtils.h
/// @brief A scripted INetlinkTransport and netlink message builders for NetlinkSocketStats tests.

#if defined(__linux__) && __has_include(<linux/inet_diag.h>) && __has_include(<linux/sock_diag.h>)

#include "Platform/Linux/NetlinkSocketStats.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - Linux UAPI headers
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>
// NOLINTEND(misc-include-cleaner)

namespace Platform::TestSupport
{

using Datagram = std::vector<std::byte>;

/// One socket in a scripted dump.
struct FakeSocket
{
    std::uint32_t inode = 0;
    std::uint64_t bytesReceived = 0;
    std::uint64_t bytesSent = 0;
};

/// Append one netlink message (header + payload) to a datagram, padded to NLMSG_ALIGNTO.
inline void
appendMessage(Datagram& datagram, std::uint16_t type, std::uint32_t sequence, std::uint32_t portId, std::span<const std::byte> payload)
{
    nlmsghdr header{};
    header.nlmsg_len = static_cast<std::uint32_t>(NLMSG_LENGTH(payload.size()));
    header.nlmsg_type = type;
    header.nlmsg_flags = NLM_F_MULTI;
    header.nlmsg_seq = sequence;
    header.nlmsg_pid = portId;
    const std::size_t offset = datagram.size();
    datagram.resize(offset + NLMSG_ALIGN(header.nlmsg_len));
    std::memcpy(datagram.data() + offset, &header, sizeof(header));
    if (!payload.empty())
    {
        std::memcpy(datagram.data() + offset + NLMSG_HDRLEN, payload.data(), payload.size());
    }
}

/// An inet_diag_msg for `socket` followed by an INET_DIAG_INFO attribute holding its tcp_info.
[[nodiscard]] inline Datagram socketPayload(const FakeSocket& socket)
{
    inet_diag_msg message{};
    message.idiag_inode = socket.inode;
    tcp_info info{};
    info.tcpi_bytes_received = socket.bytesReceived;
    info.tcpi_bytes_acked = socket.bytesSent;
    rtattr attribute{};
    attribute.rta_type = INET_DIAG_INFO;
    attribute.rta_len = static_cast<unsigned short>(RTA_LENGTH(sizeof(info)));

    Datagram payload(sizeof(message) + RTA_SPACE(sizeof(info)));
    std::memcpy(payload.data(), &message, sizeof(message));
    std::memcpy(payload.data() + sizeof(message), &attribute, sizeof(attribute));
    std::memcpy(payload.data() + sizeof(message) + RTA_LENGTH(0), &info, sizeof(info));
    return payload;
}

/// A datagram holding one SOCK_DIAG_BY_FAMILY message per socket.
[[nodiscard]] inline Datagram socketsDatagram(std::span<const FakeSocket> sockets, std::uint32_t sequence, std::uint32_t portId)
{
    Datagram datagram;
    for (const auto& socket : sockets)
    {
        appendMessage(datagram, SOCK_DIAG_BY_FAMILY, sequence, portId, socketPayload(socket));
    }
    return datagram;
}

/// A datagram holding the dump's NLMSG_DONE.
[[nodiscard]] inline Datagram doneDatagram(std::uint32_t sequence, std::uint32_t portId)
{
    Datagram datagram;
    const std::int32_t status = 0;
    appendMessage(datagram, NLMSG_DONE, sequence, portId, std::as_bytes(std::span{&status, 1}));
    return datagram;
}

/// A datagram holding an NLMSG_ERROR reply carrying `error` (a negative errno).
[[nodiscard]] inline Datagram errorDatagram(std::uint32_t sequence, std::uint32_t portId, int error)
{
    nlmsgerr payload{};
    payload.error = error;
    Datagram datagram;
    appendMessage(datagram, NLMSG_ERROR, sequence, portId, std::as_bytes(std::span{&payload, 1}));
    return datagram;
}

/// Plays the kernel: each request sent is answered with whatever `onRequest` returns, queued for
/// receive(). A std::nullopt entry is a receive timeout (EAGAIN) at that point in the stream.
class ScriptedNetlinkTransport final : public INetlinkTransport
{
  public:
    static constexpr std::uint32_t PORT_ID = 4242;

    struct Request
    {
        std::uint8_t family = 0;
        std::uint8_t protocol = 0;
        std::uint32_t sequence = 0;
    };

    using Reply = std::vector<std::optional<Datagram>>;
    std::function<Reply(const Request&)> onRequest;
    std::vector<Request> requests;
    std::deque<std::optional<Datagram>> queued;

    [[nodiscard]] NetlinkIoResult send(std::span<const std::byte> request) override
    {
        if (request.size() < sizeof(nlmsghdr) + sizeof(inet_diag_req_v2))
        {
            return {.bytes = -1, .error = EINVAL};
        }
        nlmsghdr header{};
        inet_diag_req_v2 body{};
        std::memcpy(&header, request.data(), sizeof(header));
        std::memcpy(&body, request.data() + sizeof(header), sizeof(body));
        const Request parsed{.family = body.sdiag_family, .protocol = body.sdiag_protocol, .sequence = header.nlmsg_seq};
        requests.push_back(parsed);
        if (onRequest)
        {
            for (auto& datagram : onRequest(parsed))
            {
                queued.push_back(std::move(datagram));
            }
        }
        return {.bytes = static_cast<std::int64_t>(request.size()), .error = 0};
    }

    [[nodiscard]] NetlinkIoResult receive(std::span<std::byte> buffer, bool /*nonBlocking*/) override
    {
        if (queued.empty())
        {
            return {.bytes = -1, .error = EAGAIN};
        }
        std::optional<Datagram> next = std::move(queued.front());
        queued.pop_front();
        if (!next.has_value())
        {
            return {.bytes = -1, .error = EAGAIN}; // the scripted timeout
        }
        const std::size_t length = std::min(buffer.size(), next->size());
        std::memcpy(buffer.data(), next->data(), length);
        return {.bytes = static_cast<std::int64_t>(length), .error = 0};
    }

    [[nodiscard]] std::uint32_t portId() const noexcept override
    {
        return PORT_ID;
    }
};

/// A reply carrying `sockets` and the NLMSG_DONE for `request`, to this socket.
[[nodiscard]] inline ScriptedNetlinkTransport::Reply completeDump(const ScriptedNetlinkTransport::Request& request,
                                                                  std::span<const FakeSocket> sockets)
{
    ScriptedNetlinkTransport::Reply reply;
    if (!sockets.empty())
    {
        reply.emplace_back(socketsDatagram(sockets, request.sequence, ScriptedNetlinkTransport::PORT_ID));
    }
    reply.emplace_back(doneDatagram(request.sequence, ScriptedNetlinkTransport::PORT_ID));
    return reply;
}

} // namespace Platform::TestSupport

#endif
