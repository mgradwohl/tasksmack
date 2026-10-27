#include "WindowsPublicIp.h"

#include "Platform/DnsMessage.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/PublicIpLookup.h"
#include "WinString.h"

// NOLINTBEGIN(misc-include-cleaner) - Winsock umbrella headers; their symbols come from sub-headers
// that cannot be included on their own without breaking the required include order.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// clang-format on
// NOLINTEND(misc-include-cleaner)

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Platform::WindowsPublicIp
{

namespace
{

/// One WSAStartup reference for the lookup's sockets, released when it ends. Winsock counts the
/// references, so this neither starts nor stops it under the app's other users.
class WinsockScope
{
  public:
    WinsockScope() : m_Error(::WSAStartup(MAKEWORD(2, 2), &m_Data))
    {}
    WinsockScope(const WinsockScope&) = delete;
    WinsockScope& operator=(const WinsockScope&) = delete;
    WinsockScope(WinsockScope&&) = delete;
    WinsockScope& operator=(WinsockScope&&) = delete;
    ~WinsockScope()
    {
        if (m_Error == 0)
        {
            ::WSACleanup();
        }
    }

    /// 0 when Winsock started, else WSAStartup's error.
    [[nodiscard]] int error() const noexcept
    {
        return m_Error;
    }

  private:
    WSADATA m_Data{};
    int m_Error;
};

/// One query in flight: its socket, the bytes sent, and the result once there is one.
struct Query : PublicIpLookup::Outcome
{
    SOCKET socket = INVALID_SOCKET;
    std::vector<std::uint8_t> sent;

    Query() = default;
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;
    Query(Query&&) = delete;
    Query& operator=(Query&&) = delete;
    ~Query()
    {
        if (std::cmp_not_equal(socket, INVALID_SOCKET))
        {
            ::closesocket(socket);
        }
    }
};

/// The system's text for a Winsock error, without the trailing period and line break; "error N" when
/// there is none.
[[nodiscard]] std::string errorText(int wsaError)
{
    std::array<wchar_t, 512> text{};
    const DWORD length = ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                          nullptr,
                                          static_cast<DWORD>(wsaError),
                                          0,
                                          text.data(),
                                          static_cast<DWORD>(text.size()),
                                          nullptr);
    std::wstring_view view(text.data(), length);
    while (!view.empty() && (view.back() == L'\r' || view.back() == L'\n' || view.back() == L' ' || view.back() == L'.'))
    {
        view.remove_suffix(1);
    }
    return view.empty() ? std::format("error {}", wsaError) : WinString::wideToUtf8(view);
}

/// Opens a non-blocking UDP socket connected to @p resolver (so only its replies arrive) and sends it
/// a query for @p name. Sets query.done with a reason on failure.
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
    int length = 0;
    if (v6)
    {
        auto* in6 = reinterpret_cast<sockaddr_in6*>(&address); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr API
        in6->sin6_family = AF_INET6;
        in6->sin6_port = ::htons(resolver.port);
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
        in4->sin_port = ::htons(resolver.port);
        if (::inet_pton(AF_INET, resolver.address.c_str(), &in4->sin_addr) != 1)
        {
            query.fail("The resolver's address is invalid");
            return;
        }
        length = sizeof(sockaddr_in);
    }

    // Not inherited by the processes TaskSmack starts (SOCK_CLOEXEC on Linux).
    query.socket = ::WSASocketW(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (std::cmp_equal(query.socket, INVALID_SOCKET))
    {
        query.fail(Detail::networkError(::WSAGetLastError(), v6));
        return;
    }
    u_long nonBlocking = 1;
    if (::ioctlsocket(query.socket, static_cast<long>(FIONBIO), &nonBlocking) != 0)
    {
        query.fail(Detail::networkError(::WSAGetLastError(), v6));
        return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr API
    if (::connect(query.socket, reinterpret_cast<const sockaddr*>(&address), length) != 0)
    {
        query.fail(Detail::networkError(::WSAGetLastError(), v6));
        return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Winsock takes char buffers
    if (::send(query.socket, reinterpret_cast<const char*>(query.sent.data()), static_cast<int>(query.sent.size()), 0) == SOCKET_ERROR)
    {
        query.fail(Detail::networkError(::WSAGetLastError(), v6));
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
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Winsock takes char buffers
        const int received = ::recv(query.socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (received == SOCKET_ERROR)
        {
            const int error = ::WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINTR)
            {
                return; // nothing more yet
            }
            if (error == WSAEMSGSIZE)
            {
                query.fail(std::string(PublicIpLookup::UNREADABLE_REPLY)); // larger than any reply to this query
                return;
            }
            query.fail(Detail::networkError(error, v6));
            return;
        }
        const auto answer =
            DnsMessage::parseAnswer(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(received)), query.sent);
        // A stray datagram leaves the query waiting for its reply; anything else ends it.
        query.settle(PublicIpLookup::replyDisposition(answer, v6, formatAddress));
    }
}

} // namespace

std::string Detail::networkError(int wsaError, bool v6)
{
    switch (wsaError)
    {
    case WSAENETUNREACH:
    case WSAEADDRNOTAVAIL:
    case WSAEAFNOSUPPORT:
    case WSAEHOSTUNREACH:
    case WSAENETDOWN:
        return v6 ? "No IPv6 route to the internet" : "No IPv4 route to the internet";
    case WSAECONNREFUSED:
    case WSAECONNRESET: // a connected UDP socket's ICMP port unreachable (SIO_UDP_CONNRESET, on by default)
        return "The resolver refused the query";
    default:
        return std::format("Couldn't reach the resolver: {}", errorText(wsaError));
    }
}

PublicIpInfo lookUp(const Options& options)
{
    const WinsockScope winsock; // outlives the queries' sockets, declared after it
    if (winsock.error() != 0)
    {
        PublicIpLookup::Outcome v4Failed;
        v4Failed.fail(Detail::networkError(winsock.error(), false));
        PublicIpLookup::Outcome v6Failed;
        v6Failed.fail(Detail::networkError(winsock.error(), true));
        return PublicIpLookup::assembleInfo(options, v4Failed, v6Failed);
    }

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
        // Only the queries still waiting: a query not done always has its socket.
        std::array<WSAPOLLFD, 2> fds{};
        std::array<Query*, 2> polled{};
        ULONG count = 0;
        for (Query* query : {&v4, &v6})
        {
            if (!query->done)
            {
                fds.at(count) = WSAPOLLFD{.fd = query->socket, .events = POLLRDNORM, .revents = 0};
                polled.at(count) = query;
                ++count;
            }
        }
        const int ready = ::WSAPoll(fds.data(), count, static_cast<INT>(left.count()));
        if (ready == SOCKET_ERROR)
        {
            break;
        }
        for (ULONG i = 0; i < count; ++i)
        {
            // An ICMP refusal shows as POLLERR (or POLLHUP); recv() then reports it.
            if ((static_cast<unsigned>(fds.at(i).revents) & static_cast<unsigned>(POLLRDNORM | POLLERR | POLLHUP | POLLNVAL)) != 0U)
            {
                receive(*polled.at(i), polled.at(i) == &v6);
            }
        }
    }

    return PublicIpLookup::assembleInfo(options, v4, v6);
}

} // namespace Platform::WindowsPublicIp
