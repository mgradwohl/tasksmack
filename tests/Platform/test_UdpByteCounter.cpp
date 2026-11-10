// The helper's eBPF UDP byte counter (#3366). The attach plan and the embedded object are checked unprivileged;
// loading needs root, so UdpByteCounterRootTest runs only in CI's sudo step on a disposable runner.

#include "Platform/IUdpTrafficCollector.h"
#include "Platform/Linux/LinuxUdpTrafficCollector.h"
#include "Platform/Linux/UdpByteCounter.h"
#include "Support/CiRealSystem.h"
#include "UdpBytesBpfObject.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

// NOLINTBEGIN(misc-include-cleaner) - socket and process headers: include-cleaner lacks mappings for AF_*, SOCK_*
// and the W* macros
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
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
        "kprobe/udp_sendmsg",
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
        EXPECT_EQ(wanted, fexit ? (attempt.ipv6 ? 5 : 2) : (attempt.ipv6 ? 5 : 3));
    }

    const bpf_map* map = bpf_object__find_map_by_name(object.get(), "udp_bytes_by_tgid");
    ASSERT_NE(map, nullptr);
    EXPECT_EQ(bpf_map__type(map), BPF_MAP_TYPE_LRU_HASH);
    EXPECT_EQ(bpf_map__key_size(map), sizeof(std::uint32_t));
    EXPECT_EQ(bpf_map__value_size(map), 2 * sizeof(std::uint64_t));
    EXPECT_EQ(bpf_map__max_entries(map), 16384U);
}

// #3374: a failed attempt's reason quotes the end of libbpf's warnings, where the verifier says why it refused.
TEST(UdpByteCounterTest, AFailuresReasonQuotesOnlyTheEndOfWhatLibbpfSaid)
{
    EXPECT_EQ(UdpLibbpfLog::withLog("fexit: loading failed", ""), "fexit: loading failed");
    EXPECT_EQ(UdpLibbpfLog::withLog("fexit: loading failed", "\n\n"), "fexit: loading failed") << "only newlines is nothing said";
    EXPECT_EQ(UdpLibbpfLog::withLog("x", "R1 invalid mem access\n"), "x; libbpf: ...R1 invalid mem access");

    const std::string longLog = std::string(UdpLibbpfLog::QUOTED_BYTES, 'a') + "the last line\n";
    const std::string quoted = UdpLibbpfLog::withLog("x", longLog);
    const std::string_view prefix = "x; libbpf: ...";
    ASSERT_TRUE(quoted.starts_with(prefix));
    EXPECT_EQ(quoted.size(), prefix.size() + UdpLibbpfLog::QUOTED_BYTES);
    EXPECT_TRUE(quoted.ends_with("aaathe last line"));
}

TEST(UdpByteCounterTest, AnAttemptKeepsOnlyTheLastOfLibbpfsWarnings)
{
    std::string capture;
    UdpLibbpfLog::keep(capture, "first");
    UdpLibbpfLog::keep(capture, "second");
    EXPECT_EQ(capture, "first\nsecond\n");

    UdpLibbpfLog::keep(capture, std::string(UdpLibbpfLog::CAPTURE_BYTES, 'v'));
    UdpLibbpfLog::keep(capture, "why it refused");
    EXPECT_EQ(capture.size(), UdpLibbpfLog::CAPTURE_BYTES);
    EXPECT_TRUE(capture.ends_with("v\nwhy it refused\n"));
    EXPECT_FALSE(capture.contains("first")) << "the oldest warnings go first";
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
//     least 10): every one is counted, as no receive holds a slot while it waits;
//   - over IPv4 while its thread carries a stale "inside udpv6_sendmsg" marker the parent planted (as a missed
//     kretprobe would leave): still counted, as the marker names another socket;
//   - to a closed port with IP_RECVERR, then the ICMP report read from the error queue: the send counts, the
//     report (which carries the payload) doesn't.
constexpr int DATAGRAMS = 8;
constexpr std::size_t DATAGRAM_BYTES = 1000;
constexpr int MAPPED_DATAGRAMS = 4;
constexpr std::size_t MAPPED_DATAGRAM_BYTES = 500;
constexpr std::size_t BLOCKED_BYTES = 100;
constexpr std::size_t ERRQUEUE_BYTES = 50;

/// How many receivers block at once: past the default kretprobe slots on this machine.
[[nodiscard]] std::size_t blockedReceivers()
{
    return (4 * std::max(1U, std::thread::hardware_concurrency())) + 32;
}

[[nodiscard]] std::uint64_t expectedRxBytes()
{
    return (DATAGRAMS * DATAGRAM_BYTES) + (MAPPED_DATAGRAMS * MAPPED_DATAGRAM_BYTES) + (blockedReceivers() * BLOCKED_BYTES);
}

[[nodiscard]] std::uint64_t expectedTxBytes()
{
    return expectedRxBytes() + ERRQUEUE_BYTES;
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

/// Sends ERRQUEUE_BYTES to a port nothing listens on, with IP_RECVERR, and reads the ICMP report back from the
/// error queue: false if any step fails.
[[nodiscard]] bool sendToAClosedPortAndReadTheError()
{
    sockaddr_in closed{};
    closed.sin_family = AF_INET;
    closed.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    const int probe = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (!bindToItself(probe, closed) || ::close(probe) != 0) // the port it got is free again, with no listener
    {
        return false;
    }
    const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    const int on = 1;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes sockaddr*
    if (sock < 0 || ::setsockopt(sock, IPPROTO_IP, IP_RECVERR, &on, sizeof(on)) != 0 ||
        ::connect(sock, reinterpret_cast<const sockaddr*>(&closed), sizeof(closed)) != 0)
    {
        return false;
    }
    const std::array<char, ERRQUEUE_BYTES> payload{};
    if (::send(sock, payload.data(), payload.size(), 0) != static_cast<ssize_t>(ERRQUEUE_BYTES))
    {
        return false;
    }
    pollfd waiting{.fd = sock, .events = 0, .revents = 0}; // POLLERR is always reported
    if (::poll(&waiting, 1, 2000) != 1 || (waiting.revents & POLLERR) == 0)
    {
        return false;
    }
    std::array<char, ERRQUEUE_BYTES> buffer{};
    std::array<char, 512> control{};
    iovec io{.iov_base = buffer.data(), .iov_len = buffer.size()};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    return ::recvmsg(sock, &message, MSG_ERRQUEUE) == static_cast<ssize_t>(ERRQUEUE_BYTES);
}

[[noreturn]] void sendAndReceiveUdp(int go)
{
    // The parent plants its stale marker for this thread first.
    char ready = 0;
    if (::read(go, &ready, 1) != 1)
    {
        ::_exit(8);
    }
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
    if (received != receivers.size())
    {
        ::_exit(7);
    }
    ::_exit(sendToAClosedPortAndReadTheError() ? 0 : 9);
}

/// How many of this process's open descriptors link to @p target ("anon_inode:bpf-map", ...).
[[nodiscard]] std::size_t openDescriptorsOf(std::string_view target)
{
    std::size_t count = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", error))
    {
        std::error_code linkError;
        if (std::filesystem::read_symlink(entry.path(), linkError).string() == target)
        {
            ++count;
        }
    }
    return count;
}

/// A file descriptor for the newest BPF map named @p name (the counter this test loaded), or -1. Root only.
[[nodiscard]] int mapNamed(std::string_view name)
{
    int found = -1;
    std::uint32_t id = 0;
    while (bpf_map_get_next_id(id, &id) == 0)
    {
        const int fd = bpf_map_get_fd_by_id(id);
        if (fd < 0)
        {
            continue;
        }
        bpf_map_info info{};
        std::uint32_t length = sizeof(info);
        if (bpf_map_get_info_by_fd(fd, &info, &length) == 0 && name == std::string_view(info.name))
        {
            if (found >= 0)
            {
                ::close(found);
            }
            found = fd; // ids only grow: the last one found is the newest
            continue;
        }
        ::close(fd);
    }
    return found;
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
    // Only the totals map stays open: no program descriptors, and not in_udpv6_send, whose values are kernel socket
    // addresses (#3554).
    EXPECT_EQ(openDescriptorsOf("anon_inode:bpf-map"), 1U);
    EXPECT_EQ(openDescriptorsOf("anon_inode:bpf-prog"), 0U);

    std::array<int, 2> go{-1, -1};
    ASSERT_EQ(::pipe(go.data()), 0);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        ::close(go[1]);
        sendAndReceiveUdp(go[0]);
    }
    ::close(go[0]);
    // A marker its thread's next IPv4 sends must not be hidden by: another socket's, as a missed udpv6_sendmsg
    // return in the kprobe mode would leave.
    const int markers = mapNamed("in_udpv6_send");
    ASSERT_GE(markers, 0);
    const std::uint64_t thread = (static_cast<std::uint64_t>(child) << 32U) | static_cast<std::uint32_t>(child);
    const std::uint64_t otherSocket = 0xDEAD0000ULL;
    EXPECT_EQ(bpf_map_update_elem(markers, &thread, &otherSocket, BPF_ANY), 0);
    ::close(markers);
    ASSERT_EQ(::write(go[1], "g", 1), 1);
    ::close(go[1]);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    const auto read = counter->read();
    ASSERT_TRUE(read.has_value()) << read.error();
    const std::vector<UdpByteCounts>& counts = *read;
    const auto entry = std::ranges::find(counts, static_cast<std::uint32_t>(child), &UdpByteCounts::tgid);
    ASSERT_NE(entry, counts.end()) << "no entry for the child's tgid among " << counts.size();
    EXPECT_EQ(entry->txBytes, expectedTxBytes()) << "the IPv4-mapped sends once, and the IPv4 sends despite the stale marker";
    EXPECT_EQ(entry->rxBytes, expectedRxBytes()) << "every blocked receive, and no peek or error-queue read";
}

// Root's in-process collector (#3580) over the real counter, in this mode: what the child sent and received is what it
// reports for the child, as all new bytes.
TEST_P(UdpByteCounterRootTest, RootsInProcessCollectorReportsWhatTheCounterCountedForAChild)
{
    const std::array attempts{UdpAttachAttempt{.mode = GetParam(), .ipv6 = true}};
    LinuxUdpTrafficCollector collector(
        [&attempts]() -> std::expected<LinuxUdpTrafficCollector::Read, std::string>
        {
            auto loaded = UdpByteCounter::load(attempts);
            if (!loaded.has_value())
            {
                return std::unexpected(std::move(loaded.error()));
            }
            auto counter = std::make_shared<UdpByteCounter>(std::move(*loaded));
            return LinuxUdpTrafficCollector::Read{[counter] { return counter->read(); }};
        });
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started) << collector.problem();

    std::array<int, 2> go{-1, -1};
    ASSERT_EQ(::pipe(go.data()), 0);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        ::close(go[1]);
        sendAndReceiveUdp(go[0]);
    }
    ::close(go[0]);
    ASSERT_EQ(::write(go[1], "g", 1), 1);
    ::close(go[1]);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    const std::vector<UdpProcessTraffic> traffic = collector.sample();
    EXPECT_TRUE(collector.isDelivering()) << collector.problem();
    const auto entry = std::ranges::find(traffic, static_cast<std::uint32_t>(child), &UdpProcessTraffic::pid);
    ASSERT_NE(entry, traffic.end()) << "no entry for the child among " << traffic.size();
    EXPECT_EQ(entry->bytesSent, expectedTxBytes());
    EXPECT_EQ(entry->bytesReceived, expectedRxBytes());
    EXPECT_TRUE(entry->readable);
    EXPECT_FALSE(entry->baselineOnly);
    collector.stop();
    EXPECT_EQ(openDescriptorsOf("anon_inode:bpf-map"), 0U) << "stop() unloads the counter";
}

// #3374: the collector Factory builds as root, with the counter's own attach plan, loads and unloads the real counter.
// Not per mode, so outside CI's PR root step: the coverage job's root pass runs it (tools/coverage.sh).
class UdpTrafficCollectorRootTest : public ::TestSupport::CiRealSystemTest
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
            GTEST_SKIP() << "loading the BPF program needs root: the coverage job runs this suite under sudo";
        }
    }
};

TEST_F(UdpTrafficCollectorRootTest, TheDefaultCollectorLoadsTheCounterAndStopUnloadsIt)
{
    LinuxUdpTrafficCollector collector;
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started) << collector.problem();
    EXPECT_FALSE(collector.unsupported());
    EXPECT_TRUE(collector.problem().empty());
    EXPECT_EQ(openDescriptorsOf("anon_inode:bpf-map"), 1U);
    collector.stop();
    EXPECT_EQ(openDescriptorsOf("anon_inode:bpf-map"), 0U) << "stop() unloads the counter";
}

INSTANTIATE_TEST_SUITE_P(EachMode,
                         UdpByteCounterRootTest,
                         ::testing::Values(UdpAttachMode::Fexit, UdpAttachMode::Kretprobe),
                         [](const ::testing::TestParamInfo<UdpAttachMode>& info)
                         { return info.param == UdpAttachMode::Fexit ? std::string("Fexit") : std::string("Kretprobe"); });

} // namespace
} // namespace Platform
