/// @file test_WindowsPublicIp.cpp
/// @brief The Windows public IP lookup (#1894), the Winsock twin of test_LinuxPublicIp.cpp, against a
/// fake resolver on 127.0.0.1 and ::1 (nothing goes off the machine): both addresses found at once, a
/// stray datagram ignored, replies with another ID only (the timeout), a resolver that never answers (the
/// timeout), one that isn't listening (refused), a malformed reply, an error reply, an invalid resolver
/// address, and the Winsock errors' reasons.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WindowsPublicIp.h"

#include <gtest/gtest.h>

// NOLINTBEGIN(misc-include-cleaner) - Winsock umbrella headers: include-cleaner lacks mappings for their types
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// clang-format on
// NOLINTEND(misc-include-cleaner)

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Platform
{
namespace
{

using Bytes = std::vector<std::uint8_t>;

/// How the fake resolver answers a query.
enum class Behaviour : std::uint8_t
{
    Answer,          ///< With the address
    StrayThenAnswer, ///< First a datagram with another ID, then the address
    OtherIdOnly,     ///< Only replies with another ID
    Silent,          ///< Never
    ServerFailure,   ///< SERVFAIL
    NameNotFound,    ///< NXDOMAIN
    NoRecords,       ///< A good reply with no answer in it
    Truncated,       ///< The TC flag set: the reply couldn't fit
    Garbled,         ///< The answer's address cut short: a record past the end
};

/// A UDP DNS server on a loopback address that answers one query per datagram, from a thread.
class FakeResolver
{
  public:
    /// @param v6 Bound to ::1, else 127.0.0.1. ok() is false when the address can't be bound.
    FakeResolver(bool v6, Behaviour behaviour, Bytes address) : m_Behaviour(behaviour), m_Address(std::move(address))
    {
        WSADATA data{};
        m_Started = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!m_Started)
        {
            return;
        }
        m_Socket = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (std::cmp_equal(m_Socket, INVALID_SOCKET))
        {
            return;
        }
        sockaddr_storage bound{};
        int length = 0;
        if (v6)
        {
            auto* in6 = reinterpret_cast<sockaddr_in6*>(&bound); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr API
            in6->sin6_family = AF_INET6;
            in6->sin6_addr = in6addr_loopback;
            length = sizeof(sockaddr_in6);
        }
        else
        {
            auto* in4 = reinterpret_cast<sockaddr_in*>(&bound); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr API
            in4->sin_family = AF_INET;
            in4->sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
            length = sizeof(sockaddr_in);
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr API
        if (::bind(m_Socket, reinterpret_cast<sockaddr*>(&bound), length) != 0 ||
            ::getsockname(m_Socket, reinterpret_cast<sockaddr*>(&bound), &length) !=
                0) // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        {
            ::closesocket(m_Socket);
            m_Socket = INVALID_SOCKET;
            return;
        }
        m_Port = ::ntohs(v6 ? reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                            : reinterpret_cast<sockaddr_in*>(&bound)->sin_port); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        m_Thread = std::thread([this] { serve(); });
    }

    FakeResolver(const FakeResolver&) = delete;
    FakeResolver& operator=(const FakeResolver&) = delete;
    FakeResolver(FakeResolver&&) = delete;
    FakeResolver& operator=(FakeResolver&&) = delete;

    ~FakeResolver()
    {
        m_Stop = true;
        if (m_Thread.joinable())
        {
            m_Thread.join();
        }
        if (std::cmp_not_equal(m_Socket, INVALID_SOCKET))
        {
            ::closesocket(m_Socket);
        }
        if (m_Started)
        {
            ::WSACleanup();
        }
    }

    [[nodiscard]] bool ok() const noexcept
    {
        return std::cmp_not_equal(m_Socket, INVALID_SOCKET);
    }

    [[nodiscard]] std::uint16_t port() const noexcept
    {
        return m_Port;
    }

    [[nodiscard]] int queries() const noexcept
    {
        return m_Queries.load();
    }

  private:
    void serve()
    {
        std::array<std::uint8_t, 512> buffer{};
        while (!m_Stop)
        {
            // Polled with a short timeout, so the destructor's stop is seen promptly.
            WSAPOLLFD ready{.fd = m_Socket, .events = POLLRDNORM, .revents = 0};
            if (::WSAPoll(&ready, 1, 20) <= 0)
            {
                continue;
            }
            sockaddr_storage from{};
            int fromLength = sizeof(from);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr API and char buffers
            const int received = ::recvfrom(m_Socket,
                                            reinterpret_cast<char*>(buffer.data()),
                                            static_cast<int>(buffer.size()),
                                            0,
                                            reinterpret_cast<sockaddr*>(&from), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                                            &fromLength);
            if (received <= 12) // also an earlier reply's ICMP refusal (WSAECONNRESET)
            {
                continue;
            }
            ++m_Queries;
            const Bytes query(buffer.begin(), buffer.begin() + received);
            const auto send = [&](const Bytes& bytes)
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr API and char buffers
                ::sendto(m_Socket,
                         reinterpret_cast<const char*>(bytes.data()),
                         static_cast<int>(bytes.size()),
                         0,
                         reinterpret_cast<sockaddr*>(&from), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                         fromLength);
            };
            const auto otherId = [&]
            {
                Bytes stray = reply(query, 0, m_Address);
                stray[1] = static_cast<std::uint8_t>(stray[1] ^ 0xFFU);
                return stray;
            };
            switch (m_Behaviour)
            {
            case Behaviour::Silent:
                break;
            case Behaviour::ServerFailure:
                send(reply(query, /*rcode=*/2, {}));
                break;
            case Behaviour::NameNotFound:
                send(reply(query, /*rcode=*/3, {}));
                break;
            case Behaviour::NoRecords:
                send(reply(query, 0, {}));
                break;
            case Behaviour::Truncated:
            {
                Bytes truncated = reply(query, 0, m_Address);
                truncated[2] = static_cast<std::uint8_t>(truncated[2] | 0x02U);
                send(truncated);
                break;
            }
            case Behaviour::Garbled:
            {
                Bytes garbled = reply(query, 0, m_Address);
                garbled.resize(garbled.size() - 2);
                send(garbled);
                break;
            }
            case Behaviour::StrayThenAnswer:
                send(otherId());
                send(reply(query, 0, m_Address));
                break;
            case Behaviour::OtherIdOnly:
                send(otherId());
                break;
            case Behaviour::Answer:
                send(reply(query, 0, m_Address));
                break;
            }
        }
    }

    /// A reply to @p query with @p rcode and, when @p address isn't empty, one answer of the type asked.
    [[nodiscard]] static Bytes reply(const Bytes& query, std::uint8_t rcode, const Bytes& address)
    {
        Bytes out(query);
        out[2] = static_cast<std::uint8_t>(out[2] | 0x80U);
        out[3] = static_cast<std::uint8_t>(0x80U | rcode);
        if (rcode != 0 || address.empty())
        {
            return out;
        }
        out[7] = 1;
        const auto type = static_cast<std::uint16_t>((static_cast<unsigned>(query[query.size() - 4]) << 8U) | query[query.size() - 3]);
        out.insert(out.end(), {0xC0, 0x0C, static_cast<std::uint8_t>(type >> 8U), static_cast<std::uint8_t>(type & 0xFFU), 0x00, 0x01});
        out.insert(out.end(), {0x00, 0x00, 0x00, 0x3C, 0x00, static_cast<std::uint8_t>(address.size())});
        out.insert(out.end(), address.begin(), address.end());
        return out;
    }

    bool m_Started = false;
    SOCKET m_Socket = INVALID_SOCKET;
    std::uint16_t m_Port = 0;
    Behaviour m_Behaviour;
    Bytes m_Address;
    std::atomic<bool> m_Stop{false};
    std::atomic<int> m_Queries{0};
    std::thread m_Thread;
};

/// The addresses the fake answers with.
[[nodiscard]] Bytes v4Address()
{
    return {203, 0, 113, 7};
}

[[nodiscard]] Bytes v6Address()
{
    return {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7};
}

/// Options pointing at @p v4Port and @p v6Port on loopback.
[[nodiscard]] WindowsPublicIp::Options
options(std::uint16_t v4Port, std::uint16_t v6Port, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000))
{
    return {
        .v4Resolver = {.address = "127.0.0.1", .port = v4Port},
        .v6Resolver = {.address = "::1", .port = v6Port},
        .name = "myip.opendns.com",
        .timeout = timeout,
        .source = "the fake",
    };
}

/// A loopback port nothing listens on: a fake bound, then closed again.
[[nodiscard]] std::uint16_t closedPort(bool v6)
{
    const FakeResolver closed(v6, Behaviour::Silent, {});
    return closed.ok() ? closed.port() : 0;
}

TEST(WindowsPublicIpTest, FindsBothAddressesAtOnce)
{
    const FakeResolver v4(false, Behaviour::Answer, v4Address());
    const FakeResolver v6(true, Behaviour::Answer, v6Address());
    ASSERT_TRUE(v4.ok());
    if (!v6.ok())
    {
        GTEST_SKIP() << "no IPv6 loopback here";
    }
    const PublicIpInfo info = WindowsPublicIp::lookUp(options(v4.port(), v6.port()));
    EXPECT_TRUE(info.read);
    EXPECT_EQ(info.v4, "203.0.113.7");
    EXPECT_EQ(info.v6, "2001:db8::7");
    EXPECT_TRUE(info.v4Unavailable.empty()) << info.v4Unavailable;
    EXPECT_TRUE(info.v6Unavailable.empty()) << info.v6Unavailable;
    EXPECT_EQ(info.source, "the fake");
    EXPECT_EQ(v4.queries(), 1);
    EXPECT_EQ(v6.queries(), 1);
}

TEST(WindowsPublicIpTest, IgnoresAStrayDatagram)
{
    const FakeResolver v4(false, Behaviour::StrayThenAnswer, v4Address());
    ASSERT_TRUE(v4.ok());
    const PublicIpInfo info = WindowsPublicIp::lookUp(options(v4.port(), closedPort(true), std::chrono::milliseconds(1000)));
    EXPECT_EQ(info.v4, "203.0.113.7");
}

TEST(WindowsPublicIpTest, RepliesWithAnotherIdAreNotTheAnswer)
{
    const FakeResolver v4(false, Behaviour::OtherIdOnly, v4Address());
    const FakeResolver v6(true, Behaviour::OtherIdOnly, v6Address());
    ASSERT_TRUE(v4.ok());
    if (!v6.ok())
    {
        GTEST_SKIP() << "no IPv6 loopback here";
    }
    const PublicIpInfo info = WindowsPublicIp::lookUp(options(v4.port(), v6.port(), std::chrono::milliseconds(300)));
    EXPECT_TRUE(info.v4.empty());
    EXPECT_TRUE(info.v6.empty());
    EXPECT_EQ(info.v4Unavailable, "The resolver didn't answer within 1 s");
    EXPECT_EQ(info.v6Unavailable, "The resolver didn't answer within 1 s");
    EXPECT_EQ(v4.queries(), 1);
    EXPECT_EQ(v6.queries(), 1);
}

TEST(WindowsPublicIpTest, ASilentResolverTimesOutAndOneNotListeningRefuses)
{
    const FakeResolver silent(false, Behaviour::Silent, v4Address());
    ASSERT_TRUE(silent.ok());
    const std::uint16_t closed = closedPort(true);
    if (closed == 0)
    {
        GTEST_SKIP() << "no IPv6 loopback here";
    }
    const auto started = std::chrono::steady_clock::now();
    const PublicIpInfo info = WindowsPublicIp::lookUp(options(silent.port(), closed, std::chrono::milliseconds(300)));
    const auto took = std::chrono::steady_clock::now() - started;
    EXPECT_TRUE(info.v4.empty());
    EXPECT_EQ(info.v4Unavailable, "The resolver didn't answer within 1 s");
    EXPECT_TRUE(info.v6.empty());
    EXPECT_EQ(info.v6Unavailable, "The resolver refused the query");
    EXPECT_GE(took, std::chrono::milliseconds(250));
    EXPECT_LT(took, std::chrono::seconds(5));
}

TEST(WindowsPublicIpTest, AMalformedReplyAndAnErrorReply)
{
    const FakeResolver garbled(false, Behaviour::Garbled, v4Address());
    const FakeResolver failing(true, Behaviour::ServerFailure, {});
    ASSERT_TRUE(garbled.ok());
    if (!failing.ok())
    {
        GTEST_SKIP() << "no IPv6 loopback here";
    }
    const PublicIpInfo info = WindowsPublicIp::lookUp(options(garbled.port(), failing.port(), std::chrono::milliseconds(1000)));
    EXPECT_TRUE(info.v4.empty());
    EXPECT_EQ(info.v4Unavailable, "The resolver's reply couldn't be read");
    EXPECT_EQ(info.v6Unavailable, "The resolver answered with an error");
}

TEST(WindowsPublicIpTest, EachReplyWithoutAnAddressSaysWhy)
{
    const FakeResolver notFound(false, Behaviour::NameNotFound, {});
    const FakeResolver noRecords(true, Behaviour::NoRecords, {});
    ASSERT_TRUE(notFound.ok());
    if (!noRecords.ok())
    {
        GTEST_SKIP() << "no IPv6 loopback here";
    }
    PublicIpInfo info = WindowsPublicIp::lookUp(options(notFound.port(), noRecords.port(), std::chrono::milliseconds(1000)));
    EXPECT_EQ(info.v4Unavailable, "The resolver answered with an error");
    EXPECT_EQ(info.v6Unavailable, "No IPv6 address was returned");

    const FakeResolver noRecords4(false, Behaviour::NoRecords, {});
    const FakeResolver truncated(true, Behaviour::Truncated, v6Address());
    ASSERT_TRUE(noRecords4.ok());
    ASSERT_TRUE(truncated.ok());
    info = WindowsPublicIp::lookUp(options(noRecords4.port(), truncated.port(), std::chrono::milliseconds(1000)));
    EXPECT_EQ(info.v4Unavailable, "No IPv4 address was returned");
    EXPECT_EQ(info.v6Unavailable, "The resolver's reply couldn't be read");
}

TEST(WindowsPublicIpTest, AnInvalidResolverOrNameSendsNothing)
{
    WindowsPublicIp::Options bad = options(1, 1, std::chrono::milliseconds(500));
    bad.v4Resolver.address = "999.1.1.1";
    bad.v6Resolver.address = "not an address";
    bad.name = "my..name";
    PublicIpInfo info = WindowsPublicIp::lookUp(bad);
    EXPECT_TRUE(info.read);
    EXPECT_EQ(info.v4Unavailable, "The name to look up is invalid") << "the name is checked first";
    EXPECT_EQ(info.v6Unavailable, "The name to look up is invalid");
    bad.name = "myip.opendns.com";
    const auto started = std::chrono::steady_clock::now();
    info = WindowsPublicIp::lookUp(bad);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(400)) << "nothing to wait for";
    EXPECT_EQ(info.v4Unavailable, "The resolver's address is invalid");
    EXPECT_EQ(info.v6Unavailable, "The resolver's address is invalid");
}

TEST(WindowsPublicIpTest, NetworkErrorsNameTheirCause)
{
    using WindowsPublicIp::Detail::networkError;
    for (const int unreachable : {WSAENETUNREACH, WSAEADDRNOTAVAIL, WSAEAFNOSUPPORT, WSAEHOSTUNREACH, WSAENETDOWN})
    {
        EXPECT_EQ(networkError(unreachable, true), "No IPv6 route to the internet");
        EXPECT_EQ(networkError(unreachable, false), "No IPv4 route to the internet");
    }
    EXPECT_EQ(networkError(WSAECONNREFUSED, false), "The resolver refused the query");
    EXPECT_EQ(networkError(WSAECONNRESET, true), "The resolver refused the query");
    const std::string other = networkError(WSAEACCES, false);
    EXPECT_TRUE(other.starts_with("Couldn't reach the resolver: ")) << other;
    EXPECT_GT(other.size(), std::string("Couldn't reach the resolver: ").size()) << other;
    EXPECT_FALSE(other.ends_with('.')) << other;
    EXPECT_FALSE(other.ends_with('\n')) << other;
}

} // namespace
} // namespace Platform
