// The helper's eBPF UDP byte counter (#3366). The attach plan and the embedded object are checked unprivileged;
// loading needs root, so UdpByteCounterRootTest runs only in CI's sudo step on a disposable runner.

#include "Platform/Linux/UdpByteCounter.h"
#include "Support/CiRealSystem.h"
#include "UdpBytesBpfObject.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
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
// expects, and every attempt has all four functions' programs (two for IPv4 only).
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
        "fexit/udp_recvmsg",
        "fexit/udp_sendmsg",
        "fexit/udpv6_recvmsg",
        "fexit/udpv6_sendmsg",
        "kretprobe/udp_recvmsg",
        "kretprobe/udp_sendmsg",
        "kretprobe/udpv6_recvmsg",
        "kretprobe/udpv6_sendmsg",
    };
    EXPECT_EQ(sections, expected);

    for (const UdpAttachAttempt& attempt : udpAttachAttempts())
    {
        const auto wanted =
            std::ranges::count_if(sections, [&attempt](const std::string& section) { return udpProgramWanted(section, attempt); });
        EXPECT_EQ(wanted, attempt.ipv6 ? 4 : 2);
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
}

// A child sends datagrams to its own socket and reads them back, so its tgid both sends and receives. Each is
// one call into udp_sendmsg or udp_recvmsg.
constexpr int DATAGRAMS = 8;
constexpr std::size_t DATAGRAM_BYTES = 1000;

[[noreturn]] void sendAndReceiveUdp()
{
    const int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes sockaddr*
    if (sock < 0 || ::bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::getsockname(sock, reinterpret_cast<sockaddr*>(&address), &length) != 0)
    {
        ::_exit(2);
    }
    std::array<char, DATAGRAM_BYTES> buffer{};
    for (int i = 0; i < DATAGRAMS; ++i)
    {
        if (::sendto(sock, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
                static_cast<ssize_t>(buffer.size()) ||
            ::recv(sock, buffer.data(), buffer.size(), 0) != static_cast<ssize_t>(buffer.size()))
        {
            ::_exit(3);
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    ::_exit(0);
}

class UdpByteCounterRootTest : public ::TestSupport::CiRealSystemTest
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

TEST_F(UdpByteCounterRootTest, CountsTheBytesAChildSendsAndReceivesUnderItsTgid)
{
    auto counter = UdpByteCounter::load();
    ASSERT_TRUE(counter.has_value()) << counter.error();
    // A GitHub-hosted Ubuntu runner has BTF and a 6.x kernel, so the fexit programs attach there, with IPv6.
    EXPECT_EQ(counter->attempt(), (UdpAttachAttempt{.mode = UdpAttachMode::Fexit, .ipv6 = true}));
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
    EXPECT_EQ(entry->txBytes, DATAGRAMS * DATAGRAM_BYTES);
    EXPECT_EQ(entry->rxBytes, DATAGRAMS * DATAGRAM_BYTES);
}

} // namespace
} // namespace Platform
