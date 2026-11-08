// The helper's eBPF UDP byte counter (#3366). The attach plan and the embedded object are checked unprivileged;
// loading needs root, so UdpByteCounterRootTest runs only in CI's sudo step on a disposable runner.

#include "Platform/Linux/UdpByteCounter.h"
#include "Support/CiRealSystem.h"
#include "UdpBytesBpfObject.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <bpf/libbpf.h>

// NOLINTBEGIN(misc-include-cleaner) - socket and process headers: include-cleaner lacks mappings for AF_*, SOCK_*
// and the W* macros
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
// NOLINTEND(misc-include-cleaner)

namespace Platform
{
namespace
{

TEST(UdpByteCounterTest, TriesFexitBeforeKretprobesAndIpv6BeforeIpv4Only)
{
    const std::vector<UdpAttachAttempt> attempts(udpAttachAttempts().begin(), udpAttachAttempts().end());
    const std::vector<UdpAttachAttempt> expected{
        {.mode = UdpAttachMode::Fexit, .ipv6 = true},
        {.mode = UdpAttachMode::Fexit, .ipv6 = false},
        {.mode = UdpAttachMode::Kretprobe, .ipv6 = true},
        {.mode = UdpAttachMode::Kretprobe, .ipv6 = false},
    };
    EXPECT_EQ(attempts, expected);
}

TEST(UdpByteCounterTest, EachAttemptLoadsOnlyItsModesProgramsAndIpv6OnlyWhenAsked)
{
    constexpr UdpAttachAttempt FEXIT_BOTH{.mode = UdpAttachMode::Fexit, .ipv6 = true};
    constexpr UdpAttachAttempt FEXIT_V4{.mode = UdpAttachMode::Fexit, .ipv6 = false};
    constexpr UdpAttachAttempt KRETPROBE_BOTH{.mode = UdpAttachMode::Kretprobe, .ipv6 = true};

    EXPECT_TRUE(udpProgramWanted("fexit/udp_sendmsg", FEXIT_BOTH));
    EXPECT_TRUE(udpProgramWanted("fexit/udpv6_recvmsg", FEXIT_BOTH));
    EXPECT_TRUE(udpProgramWanted("fexit/udp_recvmsg", FEXIT_V4));
    EXPECT_FALSE(udpProgramWanted("fexit/udpv6_sendmsg", FEXIT_V4));
    EXPECT_FALSE(udpProgramWanted("kretprobe/udp_sendmsg", FEXIT_BOTH));
    EXPECT_FALSE(udpProgramWanted("fexit/udp_sendmsg", KRETPROBE_BOTH));
    EXPECT_TRUE(udpProgramWanted("kretprobe/udpv6_sendmsg", KRETPROBE_BOTH));
    EXPECT_TRUE(udpProgramWanted("fentry/udpv6_sendmsg", FEXIT_BOTH));
    EXPECT_FALSE(udpProgramWanted("fentry/udpv6_sendmsg", FEXIT_V4));
    EXPECT_TRUE(udpProgramWanted("kprobe/skb_consume_udp", KRETPROBE_BOTH));
    EXPECT_TRUE(udpProgramWanted("kprobe/skb_consume_udp", {.mode = UdpAttachMode::Kretprobe, .ipv6 = false}));
    EXPECT_FALSE(udpProgramWanted("kprobe/skb_consume_udp", FEXIT_BOTH));
    EXPECT_FALSE(udpProgramWanted("kprobe/skb_free_udp", KRETPROBE_BOTH));
    EXPECT_FALSE(udpProgramWanted("fentry/udp_recvmsg", KRETPROBE_BOTH));
    EXPECT_FALSE(udpProgramWanted("fexit/tcp_sendmsg", FEXIT_BOTH));
    EXPECT_FALSE(udpProgramWanted("license", FEXIT_BOTH));
}

struct ObjectCloser
{
    void operator()(bpf_object* object) const noexcept
    {
        bpf_object__close(object);
    }
};

// Opening an object parses it without the kernel, so this runs unprivileged: the build embedded what the loader
// expects. Each mode has a return program for all four functions, an entry program for udpv6_sendmsg (its
// IPv4-mapped sends aren't counted twice). The kprobe mode counts both receives at skb_consume_udp's entry instead
// of at their return, where a blocked receive would hold one of the kernel's few return slots.
TEST(UdpByteCounterTest, TheEmbeddedObjectHasEachModesProgramsAndATgidKeyedMap)
{
    std::unique_ptr<bpf_object, ObjectCloser> object(
        bpf_object__open_mem(UDP_BYTES_BPF_OBJECT.data(), UDP_BYTES_BPF_OBJECT.size(), nullptr));
    ASSERT_NE(object, nullptr);

    std::set<std::string> sections;
    bpf_program* program = nullptr;
    bpf_object__for_each_program(program, object.get())
    {
        sections.emplace(bpf_program__section_name(program));
    }
    const std::set<std::string> expected{
        "fentry/udpv6_sendmsg",
        "fexit/udp_recvmsg",
        "fexit/udp_sendmsg",
        "fexit/udpv6_recvmsg",
        "fexit/udpv6_sendmsg",
        "kprobe/skb_consume_udp",
        "kprobe/udpv6_sendmsg",
        "kretprobe/udp_sendmsg",
        "kretprobe/udpv6_sendmsg",
    };
    EXPECT_EQ(sections, expected);

    for (const UdpAttachAttempt& attempt : udpAttachAttempts())
    {
        const auto wanted =
            std::ranges::count_if(sections, [&attempt](const std::string& section) { return udpProgramWanted(section, attempt); });
        const bool fexit = attempt.mode == UdpAttachMode::Fexit;
        EXPECT_EQ(wanted, fexit ? (attempt.ipv6 ? 5 : 2) : (attempt.ipv6 ? 4 : 2));
    }

    const bpf_map* map = bpf_object__find_map_by_name(object.get(), "udp_bytes_by_tgid");
    ASSERT_NE(map, nullptr);
    EXPECT_EQ(bpf_map__type(map), BPF_MAP_TYPE_LRU_HASH);
    EXPECT_EQ(bpf_map__key_size(map), sizeof(std::uint32_t));
    EXPECT_EQ(bpf_map__value_size(map), 2 * sizeof(std::uint64_t));
    EXPECT_EQ(bpf_map__max_entries(map), 16384U);
}

TEST(UdpByteCounterTest, WithoutRootLoadingFailsAndSaysWhy)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "running as root, where loading can succeed";
    }
    const auto counter = UdpByteCounter::load();
    ASSERT_FALSE(counter.has_value());
    EXPECT_FALSE(counter.error().empty());
    std::cout << "[ NOTE     ] " << counter.error() << '\n'; // what a user without the helper's root would see
}

// A child sends datagrams to its own sockets and reads them back, so its tgid both sends and receives:
//   - over IPv4, each datagram peeked at (MSG_PEEK, not counted: it stays queued) and then received;
//   - from an IPv6 socket to its own IPv4-mapped address, which udpv6_sendmsg hands to udp_sendmsg: counted once;
//   - to many threads already blocked in recv(), more than the kernel's default kretprobe slots (2 per CPU, at
//     least 10): every one is counted, as no receive holds a slot while it waits.
constexpr int DATAGRAMS = 8;
constexpr std::size_t DATAGRAM_BYTES = 1000;
constexpr int MAPPED_DATAGRAMS = 4;
constexpr std::size_t MAPPED_DATAGRAM_BYTES = 500;
constexpr std::size_t BLOCKED_BYTES = 100;

/// How many receivers block at once: past the default kretprobe slots on this machine.
[[nodiscard]] std::size_t blockedReceivers()
{
    return (4 * std::max(1U, std::thread::hardware_concurrency())) + 32;
}

[[nodiscard]] std::uint64_t expectedBytes()
{
    return (DATAGRAMS * DATAGRAM_BYTES) + (MAPPED_DATAGRAMS * MAPPED_DATAGRAM_BYTES) + (blockedReceivers() * BLOCKED_BYTES);
}

/// Binds @p sock to @p address (port 0) and reads back the port it got; false on a failure.
template<typename Address> [[nodiscard]] bool bindToItself(int sock, Address& address)
{
    socklen_t length = sizeof(address);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes sockaddr*
    return sock >= 0 && ::bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
           ::getsockname(sock, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
}

/// Sends @p count datagrams of @p bytes to @p sock's own @p address and receives each, peeking first when asked.
template<typename Address> [[nodiscard]] bool echo(int sock, const Address& address, int count, std::size_t bytes, bool peekFirst)
{
    std::array<char, DATAGRAM_BYTES> buffer{};
    const auto size = static_cast<ssize_t>(bytes);
    for (int i = 0; i < count; ++i)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes sockaddr*
        if (::sendto(sock, buffer.data(), bytes, 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != size ||
            (peekFirst && ::recv(sock, buffer.data(), bytes, MSG_PEEK) != size) || ::recv(sock, buffer.data(), bytes, 0) != size)
        {
            return false;
        }
    }
    return true;
}

[[noreturn]] void sendAndReceiveUdp()
{
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    v4.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    const int sock4 = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (!bindToItself(sock4, v4) || !echo(sock4, v4, DATAGRAMS, DATAGRAM_BYTES, true))
    {
        ::_exit(2);
    }
    sockaddr_in6 mapped{};
    mapped.sin6_family = AF_INET6;
    if (::inet_pton(AF_INET6, "::ffff:127.0.0.1", &mapped.sin6_addr) != 1)
    {
        ::_exit(3);
    }
    const int sock6 = ::socket(AF_INET6, SOCK_DGRAM, 0);
    const int off = 0;
    if (sock6 < 0 || ::setsockopt(sock6, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) != 0 || !bindToItself(sock6, mapped) ||
        !echo(sock6, mapped, MAPPED_DATAGRAMS, MAPPED_DATAGRAM_BYTES, false))
    {
        ::_exit(4);
    }
    // The receivers block first; then one datagram for each.
    std::vector<int> receivers;
    std::vector<sockaddr_in> addresses;
    for (std::size_t i = 0; i < blockedReceivers(); ++i)
    {
        sockaddr_in address = v4;
        address.sin_port = 0;
        const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (!bindToItself(sock, address))
        {
            ::_exit(5);
        }
        receivers.push_back(sock);
        addresses.push_back(address);
    }
    std::atomic<std::size_t> received{0};
    std::vector<std::jthread> threads;
    for (const int sock : receivers)
    {
        threads.emplace_back(
            [sock, &received]
            {
                std::array<char, BLOCKED_BYTES> buffer{};
                if (::recv(sock, buffer.data(), buffer.size(), 0) == static_cast<ssize_t>(BLOCKED_BYTES))
                {
                    ++received;
                }
            });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{300}); // every receiver is in recv() by now
    const std::array<char, BLOCKED_BYTES> payload{};
    for (const sockaddr_in& address : addresses)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes sockaddr*
        if (::sendto(sock4, payload.data(), payload.size(), 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
            static_cast<ssize_t>(BLOCKED_BYTES))
        {
            ::_exit(6);
        }
    }
    threads.clear(); // joins them
    ::_exit(received == receivers.size() ? 0 : 7);
}

class UdpByteCounterRootTest : public ::TestSupport::CiRealSystemTest, public ::testing::WithParamInterface<UdpAttachMode>
{
  protected:
    void SetUp() override
    {
        ::TestSupport::CiRealSystemTest::SetUp();
        if (IsSkipped())
        {
            return;
        }
        if (::geteuid() != 0)
        {
            GTEST_SKIP() << "loading the BPF program needs root: CI runs this suite under sudo";
        }
    }
};

// Each mode on its own: a GitHub-hosted Ubuntu runner has BTF, a 6.x kernel and IPv6, so both attach there.
TEST_P(UdpByteCounterRootTest, CountsTheBytesAChildSendsAndReceivesOnceEach)
{
    const std::array attempts{UdpAttachAttempt{.mode = GetParam(), .ipv6 = true}};
    auto counter = UdpByteCounter::load(attempts);
    ASSERT_TRUE(counter.has_value()) << counter.error();
    EXPECT_EQ(counter->attempt(), attempts[0]);
    EXPECT_GE(counter->mapFd(), 0);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        sendAndReceiveUdp();
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    const auto read = counter->read();
    ASSERT_TRUE(read.has_value()) << read.error();
    const std::vector<UdpByteCounts>& counts = *read;
    const auto entry = std::ranges::find(counts, static_cast<std::uint32_t>(child), &UdpByteCounts::tgid);
    ASSERT_NE(entry, counts.end()) << "no entry for the child's tgid among " << counts.size();
    EXPECT_EQ(entry->txBytes, expectedBytes()) << "the IPv4-mapped sends must count once";
    EXPECT_EQ(entry->rxBytes, expectedBytes()) << "a peek must not count, and every blocked receive must";
}

INSTANTIATE_TEST_SUITE_P(EachMode,
                         UdpByteCounterRootTest,
                         ::testing::Values(UdpAttachMode::Fexit, UdpAttachMode::Kretprobe),
                         [](const ::testing::TestParamInfo<UdpAttachMode>& info)
                         { return info.param == UdpAttachMode::Fexit ? std::string("Fexit") : std::string("Kretprobe"); });

} // namespace
} // namespace Platform
