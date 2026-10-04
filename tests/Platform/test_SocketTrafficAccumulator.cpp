// Tests for SocketTrafficAccumulator: per-process network counters built from per-socket deltas (#1099).

#include "Platform/ProcessTypes.h"
#include "Platform/SocketTrafficAccumulator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace Platform
{
namespace
{

[[nodiscard]] ProcessCounters process(std::int32_t pid, std::uint64_t startTimeTicks = 1000)
{
    ProcessCounters counters;
    counters.pid = pid;
    counters.startTimeTicks = startTimeTicks;
    return counters;
}

/// Fold `sockets` in as one reading and publish it to `processes`.
void read(SocketTrafficAccumulator& accumulator, const std::vector<SocketTrafficSample>& sockets, std::vector<ProcessCounters>& processes)
{
    accumulator.addReading(sockets);
    accumulator.publish(processes);
}

TEST(SocketTrafficAccumulatorTest, FirstReadingIsTheBaseline)
{
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 5'000, .bytesSent = 700}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 0U);
    EXPECT_EQ(processes[0].netSentBytes, 0U);
}

TEST(SocketTrafficAccumulatorTest, AClosingSocketDoesNotEraseTheSurvivorsTraffic)
{
    // The process counter was the sum over its live sockets: socket 2 closing dropped the sum from
    // 6000 to 3000, and the whole process read 0 for an interval in which socket 1 moved 2000 bytes.
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 1'000}, {.key = 2, .pid = 10, .bytesReceived = 5'000}}, processes);
    const std::uint64_t first = processes[0].netReceivedBytes;

    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 3'000}}, processes); // socket 2 closed
    const std::uint64_t second = processes[0].netReceivedBytes;
    EXPECT_EQ(second - first, 2'000U) << "the interval's bytes are the survivor's delta";

    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 6'000}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes - second, 3'000U);
}

TEST(SocketTrafficAccumulatorTest, ASocketAttributedLateDoesNotSpike)
{
    // The inode-to-PID map is rebuilt every few seconds, so a socket can be in a reading before it
    // has an owner. Once attributed it used to deliver its lifetime bytes in one interval.
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 100}}, processes);
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 100}, {.key = 2, .pid = 0, .bytesReceived = 290'000'000}}, processes);
    const std::uint64_t before = processes[0].netReceivedBytes;

    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 100}, {.key = 2, .pid = 10, .bytesReceived = 290'050'000}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes - before, 50'000U) << "only the growth since the previous reading";
}

TEST(SocketTrafficAccumulatorTest, AnOwnershipFlipMovesNoLifetimeBytes)
{
    // A socket shared across fork() can be attributed to either process; a flip used to move its
    // whole byte count from one process's sum to the other's.
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10), process(20)};
    read(accumulator, {{.key = 1, .pid = 10, .bytesSent = 1'000'000}}, processes);
    read(accumulator, {{.key = 1, .pid = 10, .bytesSent = 1'000'500}}, processes);
    EXPECT_EQ(processes[0].netSentBytes, 500U);

    read(accumulator, {{.key = 1, .pid = 20, .bytesSent = 1'000'800}}, processes);
    EXPECT_EQ(processes[0].netSentBytes, 500U) << "the previous owner keeps what it was credited";
    EXPECT_EQ(processes[1].netSentBytes, 300U) << "the new owner gets only this interval's bytes";
}

TEST(SocketTrafficAccumulatorTest, ASocketOpenedWithinTheIntervalCountsAllItsBytes)
{
    // Not in the previous reading, so all its bytes were moved since then -- a short connection
    // that opens and transfers between two readings still counts.
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {}, processes);
    read(accumulator, {{.key = 7, .pid = 10, .bytesReceived = 4'096, .bytesSent = 512}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 4'096U);
    EXPECT_EQ(processes[0].netSentBytes, 512U);
}

TEST(SocketTrafficAccumulatorTest, CountersNeverDecrease)
{
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    const std::vector<std::vector<SocketTrafficSample>> readings{
        {{.key = 1, .pid = 10, .bytesReceived = 10}, {.key = 2, .pid = 10, .bytesReceived = 10}},
        {{.key = 2, .pid = 10, .bytesReceived = 50}},
        {},
        {{.key = 3, .pid = 10, .bytesReceived = 5}},
        {{.key = 3, .pid = 10, .bytesReceived = 2}}, // a key reused by a new connection: counter went back
        {{.key = 3, .pid = 10, .bytesReceived = 9}},
    };
    std::uint64_t previous = 0;
    for (const auto& reading : readings)
    {
        read(accumulator, reading, processes);
        EXPECT_GE(processes[0].netReceivedBytes, previous);
        previous = processes[0].netReceivedBytes;
    }
    EXPECT_EQ(previous, 40U + 5U + 7U);
}

TEST(SocketTrafficAccumulatorTest, PublishWithoutANewReadingRepeatsTheTotals)
{
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 0}}, processes);
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 800}}, processes);

    std::vector again{process(10)};
    accumulator.publish(again);
    EXPECT_EQ(again[0].netReceivedBytes, 800U);
}

TEST(SocketTrafficAccumulatorTest, AReusedPidStartsFromZero)
{
    SocketTrafficAccumulator accumulator;
    std::vector oldProcess{process(10, 1000)};
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 0}}, oldProcess);
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 800}}, oldProcess);
    ASSERT_EQ(oldProcess[0].netReceivedBytes, 800U);

    std::vector newProcess{process(10, 2000)}; // same PID, later start: a different process
    read(accumulator, {{.key = 2, .pid = 10, .bytesReceived = 30}}, newProcess);
    EXPECT_EQ(newProcess[0].netReceivedBytes, 30U);
}

TEST(SocketTrafficAccumulatorTest, UnattributedTrafficIsNotCredited)
{
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {{.key = 1, .pid = 0}}, processes);
    read(accumulator, {{.key = 1, .pid = 0, .bytesReceived = 1'000}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 0U);
}

TEST(SocketTrafficAccumulatorTest, TotalsSaturateInsteadOfWrapping)
{
    constexpr auto MAX_BYTES = std::numeric_limits<std::uint64_t>::max();
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {}, processes);
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = MAX_BYTES - 1}, {.key = 2, .pid = 10, .bytesReceived = 10}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, MAX_BYTES);
}

TEST(SocketTrafficAccumulatorTest, ResetForgetsEverything)
{
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {{.key = 1, .pid = 10}}, processes);
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 100}}, processes);
    accumulator.reset();
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 500}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 0U) << "after reset the next reading is a baseline again";
}

} // namespace
} // namespace Platform
