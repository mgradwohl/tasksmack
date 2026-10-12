/// @file test_WindowsEtwNetMath.cpp
/// @brief WindowsEtwNetMath.h (#1492), the pure parts of the opt-in UDP event trace: a
/// Microsoft-Windows-Kernel-Network UDP event's payload decoded at its template's fixed offsets
/// (IPv4 and IPv6, send and receive; TCP, short and PID-0 events ignored), the per-process byte
/// sums and their idle pruning, the synthetic socket samples and their keys, and how those
/// samples come out of Domain::SocketTrafficAccumulator; then the session name and the
/// stale-session decision.
///
/// No Windows header, so these build and run on every platform (#1133).

#include "Domain/SocketTrafficAccumulator.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/ProcessTypes.h"
#include "Platform/Windows/WindowsEtwNetMath.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Platform::EtwNet
{
namespace
{

/// A UDP event's payload as the provider lays it out: PID and size, then the addresses, ports,
/// sequence number and connection ID, filled with a recognisable pattern.
std::vector<std::byte> udpPayload(std::uint16_t eventId, std::uint32_t pid, std::uint32_t size)
{
    std::vector<std::byte> payload(udpPayloadSize(eventId), std::byte{0xAB});
    std::memcpy(payload.data(), &pid, sizeof(pid));
    std::memcpy(std::span(payload).subspan(sizeof(pid)).data(), &size, sizeof(size));
    return payload;
}

// ---------------------------------------------------------------------------
// parseUdpEvent
// ---------------------------------------------------------------------------

TEST(EtwNetParseTest, EachUdpEventGivesItsProcessSizeAndDirection)
{
    struct Case
    {
        std::uint16_t id;
        UdpDirection direction;
    };
    constexpr std::array cases{
        Case{.id = UDP4_SEND_EVENT_ID, .direction = UdpDirection::Sent},
        Case{.id = UDP4_RECV_EVENT_ID, .direction = UdpDirection::Received},
        Case{.id = UDP6_SEND_EVENT_ID, .direction = UdpDirection::Sent},
        Case{.id = UDP6_RECV_EVENT_ID, .direction = UdpDirection::Received},
    };
    for (const Case& c : cases)
    {
        const std::vector<std::byte> payload = udpPayload(c.id, 4321, 1'350);
        const std::optional<UdpPacket> packet = parseUdpEvent(c.id, payload);
        ASSERT_TRUE(packet.has_value()) << c.id;
        // value_or: clang-tidy's optional dataflow doesn't model ASSERT_TRUE's early return.
        const UdpPacket parsed = packet.value_or(UdpPacket{});
        EXPECT_EQ(parsed.pid, 4321U) << c.id;
        EXPECT_EQ(parsed.bytes, 1'350U) << c.id;
        EXPECT_EQ(parsed.direction, c.direction) << c.id;
    }
}

TEST(EtwNetParseTest, TheTemplatesAreTheManifestsSizes)
{
    // PID, size, two IPv4 (UInt32) or IPv6 (16-byte) addresses, two ports, seqnum, connid.
    EXPECT_EQ(UDP4_PAYLOAD_SIZE, 28U);
    EXPECT_EQ(UDP6_PAYLOAD_SIZE, 52U);
    EXPECT_EQ(udpPayloadSize(UDP4_SEND_EVENT_ID), UDP4_PAYLOAD_SIZE);
    EXPECT_EQ(udpPayloadSize(UDP6_RECV_EVENT_ID), UDP6_PAYLOAD_SIZE);
    EXPECT_EQ(udpPayloadSize(10), 0U); // TCPv4 send
}

TEST(EtwNetParseTest, TcpAndOtherEventsAreIgnored)
{
    // The provider's TCP send/receive (10/11, 26/27) and connect events: EStats counts TCP.
    const std::vector<std::byte> payload(64, std::byte{1});
    for (const std::uint16_t id : {std::uint16_t{10}, std::uint16_t{11}, std::uint16_t{12}, std::uint16_t{26}, std::uint16_t{27}})
    {
        EXPECT_FALSE(parseUdpEvent(id, payload).has_value()) << id;
        EXPECT_FALSE(udpEventDirection(id).has_value()) << id;
    }
}

TEST(EtwNetParseTest, AShortPayloadIsIgnored)
{
    std::vector<std::byte> v4 = udpPayload(UDP4_SEND_EVENT_ID, 7, 10);
    v4.pop_back();
    EXPECT_FALSE(parseUdpEvent(UDP4_SEND_EVENT_ID, v4).has_value());
    // An IPv4-sized payload is too short for an IPv6 event.
    EXPECT_FALSE(parseUdpEvent(UDP6_SEND_EVENT_ID, udpPayload(UDP4_SEND_EVENT_ID, 7, 10)).has_value());
    EXPECT_FALSE(parseUdpEvent(UDP4_RECV_EVENT_ID, {}).has_value());
}

TEST(EtwNetParseTest, ALongerPayloadKeepsPidAndSizeFirst)
{
    // A later template version that appends fields still has PID and size at offsets 0 and 4.
    std::vector<std::byte> payload = udpPayload(UDP6_RECV_EVENT_ID, 99, 512);
    payload.resize(payload.size() + 16, std::byte{0xFF});
    const std::optional<UdpPacket> packet = parseUdpEvent(UDP6_RECV_EVENT_ID, payload);
    ASSERT_TRUE(packet.has_value());
    EXPECT_EQ(packet.value_or(UdpPacket{}).pid, 99U);
    EXPECT_EQ(packet.value_or(UdpPacket{}).bytes, 512U);
}

TEST(EtwNetParseTest, PidZeroIsNotAttributed)
{
    EXPECT_FALSE(parseUdpEvent(UDP4_RECV_EVENT_ID, udpPayload(UDP4_RECV_EVENT_ID, 0, 100)).has_value());
}

// ---------------------------------------------------------------------------
// UdpByteCounters
// ---------------------------------------------------------------------------

TEST(EtwNetCountersTest, BytesAddUpPerProcessAndDirection)
{
    UdpByteCounters counters;
    counters.add({.pid = 10, .bytes = 100, .direction = UdpDirection::Sent});
    counters.add({.pid = 10, .bytes = 50, .direction = UdpDirection::Received});
    counters.add({.pid = 10, .bytes = 25, .direction = UdpDirection::Sent});
    counters.add({.pid = 20, .bytes = 7, .direction = UdpDirection::Received});

    std::vector<UdpProcessTraffic> totals = counters.sample();
    std::ranges::sort(totals, {}, &UdpProcessTraffic::pid);
    ASSERT_EQ(totals.size(), 2U);
    EXPECT_EQ(totals[0].pid, 10U);
    EXPECT_EQ(totals[0].bytesSent, 125U);
    EXPECT_EQ(totals[0].bytesReceived, 50U);
    EXPECT_EQ(totals[1].pid, 20U);
    EXPECT_EQ(totals[1].bytesSent, 0U);
    EXPECT_EQ(totals[1].bytesReceived, 7U);

    // Cumulative: the next sample still has them, plus anything new.
    counters.add({.pid = 20, .bytes = 3, .direction = UdpDirection::Received});
    totals = counters.sample();
    std::ranges::sort(totals, {}, &UdpProcessTraffic::pid);
    ASSERT_EQ(totals.size(), 2U);
    EXPECT_EQ(totals[0].bytesSent, 125U);
    EXPECT_EQ(totals[1].bytesReceived, 10U);
}

TEST(EtwNetCountersTest, AnIdleProcessIsForgottenAndStartsOverFromZero)
{
    UdpByteCounters counters;
    counters.add({.pid = 10, .bytes = 100, .direction = UdpDirection::Sent});
    EXPECT_EQ(counters.sample(3).size(), 1U); // Grew this interval
    EXPECT_EQ(counters.sample(3).size(), 1U); // Idle 1
    EXPECT_EQ(counters.sample(3).size(), 1U); // Idle 2
    EXPECT_TRUE(counters.sample(3).empty());  // Idle 3: forgotten
    EXPECT_EQ(counters.size(), 0U);

    counters.add({.pid = 10, .bytes = 5, .direction = UdpDirection::Sent});
    const std::vector<UdpProcessTraffic> totals = counters.sample(3);
    ASSERT_EQ(totals.size(), 1U);
    EXPECT_EQ(totals[0].bytesSent, 5U);
}

TEST(EtwNetCountersTest, NewBytesResetTheIdleCount)
{
    UdpByteCounters counters;
    counters.add({.pid = 10, .bytes = 1, .direction = UdpDirection::Sent});
    (void) counters.sample(2);
    (void) counters.sample(2); // Idle 1
    counters.add({.pid = 10, .bytes = 1, .direction = UdpDirection::Sent});
    EXPECT_EQ(counters.sample(2).size(), 1U);
    EXPECT_EQ(counters.sample(2).size(), 1U); // Idle 1 again, not 2
}

TEST(EtwNetCountersTest, ClearEmptiesIt)
{
    UdpByteCounters counters;
    counters.add({.pid = 10, .bytes = 1, .direction = UdpDirection::Sent});
    counters.clear();
    EXPECT_TRUE(counters.sample().empty());
}

// ---------------------------------------------------------------------------
// Socket samples
// ---------------------------------------------------------------------------

TEST(EtwNetSamplesTest, UdpKeysHaveBit63SetAndDifferPerProcess)
{
    EXPECT_NE(udpSocketKey(10) & (std::uint64_t{1} << 63U), 0U);
    EXPECT_NE(udpSocketKey(10), udpSocketKey(11));
    EXPECT_NE(udpSocketKey(0), 0U); // 0 is the accumulator's "no key"
}

TEST(EtwNetSamplesTest, EachProcessBecomesOneReadableConnection)
{
    std::vector<SocketTrafficSample> sockets{SocketTrafficSample{.key = 5, .pid = 10, .bytesReceived = 1, .bytesSent = 2}};
    const std::array udp{
        UdpProcessTraffic{.pid = 10, .bytesSent = 300, .bytesReceived = 400},
        UdpProcessTraffic{.pid = 20, .bytesSent = 1, .bytesReceived = 0},
    };
    appendUdpSamples(sockets, udp);
    ASSERT_EQ(sockets.size(), 3U);
    EXPECT_EQ(sockets[0].key, 5U); // The TCP rows stay as they were
    EXPECT_EQ(sockets[1].key, udpSocketKey(10));
    EXPECT_EQ(sockets[1].pid, 10);
    EXPECT_EQ(sockets[1].bytesSent, 300U);
    EXPECT_EQ(sockets[1].bytesReceived, 400U);
    EXPECT_EQ(sockets[1].ownerStartTimeTicks, 0U); // Matched by PID, as the TCP rows
    EXPECT_TRUE(sockets[1].readable);
    EXPECT_EQ(sockets[2].pid, 20);
}

/// The UDP counters through the accumulator ProcessModel uses, as readSocketTraffic() and
/// ProcessModel::refresh() feed them.
struct UdpTrafficHarness
{
    static constexpr std::int32_t PID = 77;
    static constexpr std::uint64_t SECOND_NS = 1'000'000'000ULL;

    Domain::SocketTrafficAccumulator accumulator;
    UdpByteCounters counters;
    std::uint64_t nowNs = 0;

    /// One reading: a TCP connection of the process that never moves, plus the UDP counters.
    std::pair<std::uint64_t, std::uint64_t> sample(std::uint32_t idleSamplesBeforeForget = IDLE_SAMPLES_BEFORE_FORGET)
    {
        nowNs += SECOND_NS;
        SocketTrafficReading reading{.sockets = {SocketTrafficSample{.key = 1, .pid = PID}}, .sampleTimeNs = nowNs};
        appendUdpSamples(reading.sockets, counters.sample(idleSamplesBeforeForget));
        std::vector<ProcessCounters> processes(1);
        processes[0].pid = PID;
        accumulator.apply(reading, processes);
        return {processes[0].netReceivedBytes, processes[0].netSentBytes};
    }

    void send(std::uint32_t bytes)
    {
        counters.add({.pid = static_cast<std::uint32_t>(PID), .bytes = bytes, .direction = UdpDirection::Sent});
    }
};

TEST(EtwNetSamplesTest, UdpBytesReachTheProcessTotalsInTheIntervalTheyWereSent)
{
    UdpTrafficHarness h;
    (void) h.sample(); // Baseline: no UDP yet
    h.send(1'000);
    // New since the last reading: all its bytes were sent within this interval.
    EXPECT_EQ(h.sample(), std::make_pair(std::uint64_t{0}, std::uint64_t{1'000}));
    h.send(500);
    EXPECT_EQ(h.sample(), std::make_pair(std::uint64_t{0}, std::uint64_t{1'500}));
    EXPECT_EQ(h.sample(), std::make_pair(std::uint64_t{0}, std::uint64_t{1'500})); // Quiet
}

TEST(EtwNetSamplesTest, AForgottenProcessThatSendsAgainIsCountedOnce)
{
    UdpTrafficHarness h;
    (void) h.sample(2);
    h.send(100);
    EXPECT_EQ(h.sample(2).second, 100U);
    EXPECT_EQ(h.sample(2).second, 100U); // Idle 1
    EXPECT_EQ(h.sample(2).second, 100U); // Idle 2: forgotten, out of the reading; nothing taken back
    h.send(40);                          // Starts over from 0
    EXPECT_EQ(h.sample(2).second, 140U);
}

// ---------------------------------------------------------------------------
// Session names and stale sessions
// ---------------------------------------------------------------------------

TEST(EtwNetSessionTest, TheNameRecordsTheOwnerAndParsesBack)
{
    const SessionOwner owner{.pid = 4321, .creationTime = 133'700'000'000'000'000ULL};
    const std::string name = sessionName(owner);
    EXPECT_EQ(name, "TaskSmack-UDP-4321-133700000000000000");
    EXPECT_EQ(parseSessionName(name), owner);
}

TEST(EtwNetSessionTest, OtherSessionsAreNotTaskSmacks)
{
    EXPECT_FALSE(parseSessionName("NT Kernel Logger").has_value());
    EXPECT_FALSE(parseSessionName("Circular Kernel Context Logger").has_value());
    EXPECT_FALSE(parseSessionName("TaskSmack-UDP-").has_value());
    EXPECT_FALSE(parseSessionName("TaskSmack-UDP-12").has_value());           // No creation time
    EXPECT_FALSE(parseSessionName("TaskSmack-UDP-12-").has_value());          // Empty creation time
    EXPECT_FALSE(parseSessionName("TaskSmack-UDP--5").has_value());           // Empty PID
    EXPECT_FALSE(parseSessionName("TaskSmack-UDP-1x-5").has_value());         // Not a number
    EXPECT_FALSE(parseSessionName("TaskSmack-UDP-4294967296-5").has_value()); // PID over 32 bits
    EXPECT_FALSE(parseSessionName("TaskSmack-UDP-1-18446744073709551616").has_value());
    EXPECT_FALSE(parseSessionName("taskSmack-UDP-1-1").has_value());
    EXPECT_EQ(parseSessionName("TaskSmack-UDP-4294967295-18446744073709551615"),
              (SessionOwner{.pid = 4294967295U, .creationTime = 18446744073709551615ULL}));
}

TEST(EtwNetSessionTest, OnlyAnOrphanedOrOwnSessionIsStopped)
{
    const SessionOwner self{.pid = 100, .creationTime = 5};
    const SessionOwner other{.pid = 200, .creationTime = 6};
    EXPECT_TRUE(shouldStopSession(other, self, OwnerLiveness::Gone));
    EXPECT_TRUE(shouldStopSession(other, self, OwnerLiveness::PidReused));
    EXPECT_FALSE(shouldStopSession(other, self, OwnerLiveness::Alive));   // Another TaskSmack's live session
    EXPECT_FALSE(shouldStopSession(other, self, OwnerLiveness::Unknown)); // Couldn't check: left alone
    EXPECT_TRUE(shouldStopSession(self, self, OwnerLiveness::Alive));     // This process's own leftover
    // Same PID, other creation time: an earlier TaskSmack whose PID this one reused.
    EXPECT_TRUE(shouldStopSession(SessionOwner{.pid = 100, .creationTime = 4}, self, OwnerLiveness::PidReused));
}

} // namespace
} // namespace Platform::EtwNet
