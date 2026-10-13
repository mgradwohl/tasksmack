/// @file test_LinuxPublicIp.cpp
/// @brief The public IP lookup (#1518) against a fake resolver on localhost: both addresses found at
/// once, a stray datagram ignored, a resolver that never answers (the timeout), one that isn't listening
/// (refused), an error reply, and an invalid resolver address.

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Linux/LinuxPublicIp.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

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
    Silent,          ///< Never
    ServerFailure,   ///< SERVFAIL
    NameNotFound,    ///< NXDOMAIN (#1891)
    NoRecords,       ///< A good reply with no answer in it (#1891)
    Truncated,       ///< The TC flag set: the reply couldn't fit (#1891)
};

/// A UDP DNS server on a loopback address that answers one query per datagram, from a thread.
class FakeResolver
{
  public:
    /// @param v6 Bound to ::1, else 127.0.0.1. ok() is false when the address can't be bound.
    FakeResolver(bool v6, Behaviour behaviour, Bytes address)
        : m_Fd(::socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)), m_Behaviour(behaviour), m_Address(std::move(address))
    {
        if (m_Fd < 0)
        {
            return;
        }
        sockaddr_storage bound{};
        socklen_t length = 0;
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
            in4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            length = sizeof(sockaddr_in);
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr API
        if (::bind(m_Fd, reinterpret_cast<sockaddr*>(&bound), length) != 0 ||
            ::getsockname(m_Fd, reinterpret_cast<sockaddr*>(&bound), &length) != 0) // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        {
            ::close(m_Fd);
            m_Fd = -1;
            return;
        }
        m_Port = ntohs(v6 ? reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
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
        if (m_Fd >= 0)
        {
            ::close(m_Fd);
        }
    }

    [[nodiscard]] bool ok() const noexcept
    {
        return m_Fd >= 0;
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
            pollfd ready{.fd = m_Fd, .events = POLLIN, .revents = 0};
            if (::poll(&ready, 1, 20) <= 0)
            {
                continue;
            }
            sockaddr_storage from{};
            socklen_t fromLength = sizeof(from);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr API
            const auto received = ::recvfrom(m_Fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (received <= 12)
            {
                continue;
            }
            ++m_Queries;
            const Bytes query(buffer.begin(), buffer.begin() + received);
            const auto send = [&](const Bytes& bytes)
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr API
                ::sendto(m_Fd, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&from), fromLength);
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
            case Behaviour::StrayThenAnswer:
            {
                Bytes stray = reply(query, 0, m_Address);
                stray[1] = static_cast<std::uint8_t>(stray[1] ^ 0xFFU); // another ID
                send(stray);
                send(reply(query, 0, m_Address));
                break;
            }
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
        const auto type = static_cast<std::uint16_t>((query[query.size() - 4] << 8U) | query[query.size() - 3]);
        out.insert(out.end(), {0xC0, 0x0C, static_cast<std::uint8_t>(type >> 8U), static_cast<std::uint8_t>(type & 0xFFU), 0x00, 0x01});
        out.insert(out.end(), {0x00, 0x00, 0x00, 0x3C, 0x00, static_cast<std::uint8_t>(address.size())});
        out.insert(out.end(), address.begin(), address.end());
        return out;
    }

    int m_Fd = -1;
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

/// Options pointing at @p v4 and @p v6 on loopback.
[[nodiscard]] LinuxPublicIp::Options
options(std::uint16_t v4Port, std::uint16_t v6Port, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000))
{
    return {.v4Resolver = {.address = "127.0.0.1", .port = v4Port},
            .v6Resolver = {.address = "::1", .port = v6Port},
            .name = "myip.opendns.com",
            .timeout = timeout,
            .source = "the fake"};
}

TEST(LinuxPublicIpTest, FindsBothAddressesAtOnce)
{
    const FakeResolver v4(false, Behaviour::Answer, v4Address());
    const FakeResolver v6(true, Behaviour::Answer, v6Address());
    ASSERT_TRUE(v4.ok());
    if (!v6.ok())
    {
        GTEST_SKIP() << "no IPv6 loopback here";
    }
    const PublicIpInfo info = LinuxPublicIp::lookUp(options(v4.port(), v6.port()));
    EXPECT_TRUE(info.read);
    EXPECT_EQ(info.v4, "203.0.113.7");
    EXPECT_EQ(info.v6, "2001:db8::7");
    EXPECT_TRUE(info.v4Unavailable.empty()) << info.v4Unavailable;
    EXPECT_TRUE(info.v6Unavailable.empty()) << info.v6Unavailable;
    EXPECT_EQ(info.source, "the fake");
    EXPECT_EQ(v4.queries(), 1);
    EXPECT_EQ(v6.queries(), 1);
}

TEST(LinuxPublicIpTest, IgnoresAStrayDatagram)
{
    const FakeResolver v4(false, Behaviour::StrayThenAnswer, v4Address());
    ASSERT_TRUE(v4.ok());
    const PublicIpInfo info = LinuxPublicIp::lookUp(options(v4.port(), 1, std::chrono::milliseconds(1000)));
    EXPECT_EQ(info.v4, "203.0.113.7");
}

TEST(LinuxPublicIpTest, ASilentResolverTimesOutAndOneNotListeningRefuses)
{
    const FakeResolver silent(false, Behaviour::Silent, v4Address());
    ASSERT_TRUE(silent.ok());
    std::uint16_t closedPort = 0;
    {
        const FakeResolver closed(true, Behaviour::Silent, {});
        if (!closed.ok())
        {
            GTEST_SKIP() << "no IPv6 loopback here";
        }
        closedPort = closed.port(); // closed again when it goes out of scope
    }
    const auto started = std::chrono::steady_clock::now();
    const PublicIpInfo info = LinuxPublicIp::lookUp(options(silent.port(), closedPort, std::chrono::milliseconds(300)));
    const auto took = std::chrono::steady_clock::now() - started;
    EXPECT_TRUE(info.v4.empty());
    EXPECT_EQ(info.v4Unavailable, "The resolver didn't answer within 1 s");
    EXPECT_TRUE(info.v6.empty());
    EXPECT_EQ(info.v6Unavailable, "The resolver refused the query");
    EXPECT_GE(took, std::chrono::milliseconds(250));
    EXPECT_LT(took, std::chrono::seconds(5));
}

TEST(LinuxPublicIpTest, AnErrorReplyAndAnInvalidResolver)
{
    const FakeResolver failing(false, Behaviour::ServerFailure, {});
    ASSERT_TRUE(failing.ok());
    LinuxPublicIp::Options bad = options(failing.port(), 53, std::chrono::milliseconds(1000));
    bad.v6Resolver.address = "not an address";
    const PublicIpInfo info = LinuxPublicIp::lookUp(bad);
    EXPECT_EQ(info.v4Unavailable, "The resolver answered with an error");
    EXPECT_EQ(info.v6Unavailable, "The resolver's address is invalid");
}

// #1891: each kind of reply that carries no address says why, and so do a bad resolver address and a name
// that can't be asked for.
TEST(LinuxPublicIpTest, EachReplyWithoutAnAddressSaysWhy)
{
    const FakeResolver notFound(false, Behaviour::NameNotFound, {});
    const FakeResolver noRecords(true, Behaviour::NoRecords, {});
    ASSERT_TRUE(notFound.ok());
    if (!noRecords.ok())
    {
        GTEST_SKIP() << "no IPv6 loopback here";
    }
    PublicIpInfo info = LinuxPublicIp::lookUp(options(notFound.port(), noRecords.port(), std::chrono::milliseconds(1000)));
    EXPECT_EQ(info.v4Unavailable, "The resolver answered with an error");
    EXPECT_EQ(info.v6Unavailable, "No IPv6 address was returned");

    const FakeResolver noRecords4(false, Behaviour::NoRecords, {});
    const FakeResolver truncated(true, Behaviour::Truncated, v6Address());
    ASSERT_TRUE(noRecords4.ok());
    ASSERT_TRUE(truncated.ok());
    info = LinuxPublicIp::lookUp(options(noRecords4.port(), truncated.port(), std::chrono::milliseconds(1000)));
    EXPECT_EQ(info.v4Unavailable, "No IPv4 address was returned");
    EXPECT_EQ(info.v6Unavailable, "The resolver's reply couldn't be read");

    LinuxPublicIp::Options bad = options(1, 1, std::chrono::milliseconds(500));
    bad.v4Resolver.address = "999.1.1.1";
    bad.name = "my..name";
    info = LinuxPublicIp::lookUp(bad);
    EXPECT_EQ(info.v4Unavailable, "The name to look up is invalid") << "the name is checked first";
    bad.name = "myip.opendns.com";
    info = LinuxPublicIp::lookUp(bad);
    EXPECT_EQ(info.v4Unavailable, "The resolver's address is invalid");
}

TEST(LinuxPublicIpTest, NetworkErrorsNameTheirCause)
{
    using LinuxPublicIp::Detail::networkError;
    for (const int unreachable : {ENETUNREACH, EADDRNOTAVAIL, EAFNOSUPPORT, EHOSTUNREACH})
    {
        EXPECT_EQ(networkError(unreachable, true), "No IPv6 route to the internet");
        EXPECT_EQ(networkError(unreachable, false), "No IPv4 route to the internet");
    }
    EXPECT_EQ(networkError(ECONNREFUSED, false), "The resolver refused the query");
    EXPECT_TRUE(networkError(EIO, false).starts_with("Couldn't reach the resolver: ")) << networkError(EIO, false);
}

} // namespace
} // namespace Platform
