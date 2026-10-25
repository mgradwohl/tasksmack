#include "LinuxPublicIp.h"

#include "Platform/DnsMessage.h"
#include "Platform/ISystemInfoProbe.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Platform::LinuxPublicIp
{

namespace
{

/// One query in flight: its socket, the bytes sent, and the result once there is one.
struct Query
{
    int fd = -1;
    std::vector<std::uint8_t> sent;
    bool done = false;
    std::string address;
    std::string unavailable;

    Query() = default;
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;
    Query(Query&&) = delete;
    Query& operator=(Query&&) = delete;
    ~Query()
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }

    void fail(std::string reason)
    {
        done = true;
        unavailable = std::move(reason);
    }
};

[[nodiscard]] std::string errnoText(int errorNumber)
{
    return std::strerror(errorNumber); // NOLINT(concurrency-mt-unsafe): read from one thread at a time
}

/// Opens a UDP socket connected to @p resolver (so only its replies arrive) and sends @p query a
/// query for @p name. Sets query.done with a reason on failure.
void start(Query& query, const Resolver& resolver, const std::string& name, DnsMessage::RecordType type, std::uint16_t id)
{
    const bool v6 = type == DnsMessage::RecordType::AAAA;
    query.sent = DnsMessage::buildQuery(id, name, type);
    if (query.sent.empty())
    {
        query.fail("The name to look up is invalid");
        return;
    }

    sockaddr_storage address{};
    socklen_t length = 0;
    if (v6)
    {
        auto* in6 = reinterpret_cast<sockaddr_in6*>(&address); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr API
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(resolver.port);
        if (::inet_pton(AF_INET6, resolver.address.c_str(), &in6->sin6_addr) != 1)
        {
            query.fail("The resolver's address is invalid");
            return;
        }
        length = sizeof(sockaddr_in6);
    }
    else
    {
        auto* in4 = reinterpret_cast<sockaddr_in*>(&address); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr API
        in4->sin_family = AF_INET;
        in4->sin_port = htons(resolver.port);
        if (::inet_pton(AF_INET, resolver.address.c_str(), &in4->sin_addr) != 1)
        {
            query.fail("The resolver's address is invalid");
            return;
        }
        length = sizeof(sockaddr_in);
    }

    query.fd = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (query.fd < 0)
    {
        query.fail(Detail::networkError(errno, v6));
        return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr API
    if (::connect(query.fd, reinterpret_cast<const sockaddr*>(&address), length) != 0)
    {
        query.fail(Detail::networkError(errno, v6));
        return;
    }
    if (::send(query.fd, query.sent.data(), query.sent.size(), MSG_NOSIGNAL) < 0)
    {
        query.fail(Detail::networkError(errno, v6));
    }
}

[[nodiscard]] std::string formatAddress(const std::variant<DnsMessage::Ipv4, DnsMessage::Ipv6, DnsMessage::AnswerError>& answer)
{
    std::array<char, INET6_ADDRSTRLEN> text{};
    if (const auto* v4 = std::get_if<DnsMessage::Ipv4>(&answer))
    {
        return ::inet_ntop(AF_INET, v4->data(), text.data(), text.size()) != nullptr ? std::string(text.data()) : std::string{};
    }
    if (const auto* v6 = std::get_if<DnsMessage::Ipv6>(&answer))
    {
        return ::inet_ntop(AF_INET6, v6->data(), text.data(), text.size()) != nullptr ? std::string(text.data()) : std::string{};
    }
    return {};
}

/// Reads the datagrams waiting on @p query's socket, until one is its reply or none are left.
void receive(Query& query, bool v6)
{
    std::array<std::uint8_t, 1500> buffer{};
    while (!query.done)
    {
        const auto received = ::recv(query.fd, buffer.data(), buffer.size(), 0);
        if (received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                return; // nothing more yet
            }
            query.fail(Detail::networkError(errno, v6));
            return;
        }
        const auto answer =
            DnsMessage::parseAnswer(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(received)), query.sent);
        if (const auto* error = std::get_if<DnsMessage::AnswerError>(&answer))
        {
            switch (*error)
            {
            case DnsMessage::AnswerError::NotTheReply:
                continue; // a stray datagram: keep waiting for the reply
            case DnsMessage::AnswerError::Malformed:
                query.fail("The resolver's reply couldn't be read");
                return;
            case DnsMessage::AnswerError::ServerError:
            case DnsMessage::AnswerError::NameNotFound:
                query.fail("The resolver answered with an error");
                return;
            case DnsMessage::AnswerError::NoAddress:
                query.fail(v6 ? "No IPv6 address was returned" : "No IPv4 address was returned");
                return;
            }
        }
        query.done = true;
        query.address = formatAddress(answer);
        if (query.address.empty())
        {
            query.unavailable = "The resolver's reply couldn't be read";
        }
    }
}

} // namespace

std::string Detail::networkError(int errorNumber, bool v6)
{
    switch (errorNumber)
    {
    case ENETUNREACH:
    case EADDRNOTAVAIL:
    case EAFNOSUPPORT:
    case EHOSTUNREACH:
        return v6 ? "No IPv6 route to the internet" : "No IPv4 route to the internet";
    case ECONNREFUSED:
        return "The resolver refused the query";
    default:
        return std::format("Couldn't reach the resolver: {}", errnoText(errorNumber));
    }
}

PublicIpInfo lookUp(const Options& options)
{
    std::random_device random;
    std::uniform_int_distribution<unsigned> idDistribution(0, 0xFFFF);

    Query v4;
    Query v6;
    start(v4, options.v4Resolver, options.name, DnsMessage::RecordType::A, static_cast<std::uint16_t>(idDistribution(random)));
    start(v6, options.v6Resolver, options.name, DnsMessage::RecordType::AAAA, static_cast<std::uint16_t>(idDistribution(random)));

    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    while (!v4.done || !v6.done)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0)
        {
            break;
        }
        std::array<pollfd, 2> fds{
            pollfd{.fd = v4.done ? -1 : v4.fd, .events = POLLIN, .revents = 0},
            pollfd{.fd = v6.done ? -1 : v6.fd, .events = POLLIN, .revents = 0},
        };
        const int ready = ::poll(fds.data(), fds.size(), static_cast<int>(left.count()));
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if ((fds[0].revents & (POLLIN | POLLERR)) != 0)
        {
            receive(v4, false);
        }
        if ((fds[1].revents & (POLLIN | POLLERR)) != 0)
        {
            receive(v6, true);
        }
    }

    const std::string timedOut = std::format("The resolver didn't answer within {} s", (options.timeout.count() + 999) / 1000);
    PublicIpInfo info;
    info.read = true;
    info.source = options.source;
    info.v4 = v4.address;
    info.v4Unavailable = v4.done ? v4.unavailable : timedOut;
    info.v6 = v6.address;
    info.v6Unavailable = v6.done ? v6.unavailable : timedOut;
    return info;
}

} // namespace Platform::LinuxPublicIp
