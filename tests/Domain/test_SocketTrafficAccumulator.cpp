// Tests for SocketTrafficAccumulator: per-process network counters built from per-socket deltas (#1099).

#include "Domain/SamplingConfig.h"
#include "Domain/SocketTrafficAccumulator.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace Domain
{
namespace
{

using Platform::ProcessCounters;
using Platform::SocketTrafficReading;
using Platform::SocketTrafficSample;

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

TEST(SocketTrafficAccumulatorTest, AnUnreadableSocketKeepsItsBaseline)
{
    // A socket still open whose counters couldn't be read (a failed EStats read, #1256): its byte
    // fields are ignored, it credits nothing and isn't taken as closed, and the next readable
    // sample credits all the growth since the last readable one.
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 1'000, .bytesSent = 100}}, processes); // baseline
    read(accumulator, {{.key = 1, .pid = 10, .readable = false}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 0U);
    EXPECT_EQ(processes[0].netSentBytes, 0U);
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 9'999'999, .bytesSent = 9'999'999, .readable = false}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 0U) << "an unreadable sample's byte fields are ignored";

    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 1'500, .bytesSent = 130}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 500U) << "growth since the last readable sample, not lifetime bytes";
    EXPECT_EQ(processes[0].netSentBytes, 30U);
}

TEST(SocketTrafficAccumulatorTest, ASocketFirstSeenUnreadableDoesNotCreditItsLifetimeBytes)
{
    // A socket first seen unreadable may have been open for hours: its first readable sample only
    // sets the baseline rather than crediting its lifetime bytes as new (#1256).
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 100}}, processes); // baseline
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 100}, {.key = 2, .pid = 10, .readable = false}}, processes);
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 100}, {.key = 2, .pid = 10, .readable = false}}, processes);
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 100}, {.key = 2, .pid = 10, .bytesReceived = 290'000'000}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 0U);

    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 100}, {.key = 2, .pid = 10, .bytesReceived = 290'050'000}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 50'000U);
}

TEST(SocketTrafficAccumulatorTest, AnUnreadableSocketThatClosesIsForgotten)
{
    // Unreadable keeps a socket open only while it's in the readings: once gone it's closed, and
    // a socket back under the same key afterwards is new (#1256).
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 1'000}}, processes);
    read(accumulator, {{.key = 1, .pid = 10, .readable = false}}, processes);
    read(accumulator, {}, processes);
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 40}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 40U);
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

TEST(SocketTrafficAccumulatorTest, ARollbackInOneDirectionRebaselinesBoth)
{
    // #1261 review: received going backwards means a different connection reuses the key, so its
    // sent counter isn't comparable with the old connection's either. 1000/100 -> 10/5000 must
    // credit nothing (not 4900 sent bytes), and the next reading counts growth from 10/5000.
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 1'000, .bytesSent = 100}}, processes);
    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 10, .bytesSent = 5'000}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 0U);
    EXPECT_EQ(processes[0].netSentBytes, 0U);

    read(accumulator, {{.key = 1, .pid = 10, .bytesReceived = 30, .bytesSent = 5'400}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 20U);
    EXPECT_EQ(processes[0].netSentBytes, 400U);
}

TEST(SocketTrafficAccumulatorTest, PublishTotalsFromAnOlderProcessListForgetsNothing)
{
    // #1261 review: a newer enumerate() credits a newly seen process; an older concurrent one then
    // publishes a process list without it. publishTotals() must not drop that process's totals, or
    // its next increment would publish 10 instead of 110.
    SocketTrafficAccumulator accumulator;
    std::vector newer{process(10), process(20)};
    read(accumulator, {{.key = 1, .pid = 20, .bytesReceived = 0}}, newer);
    read(accumulator, {{.key = 1, .pid = 20, .bytesReceived = 100}}, newer);
    ASSERT_EQ(newer[1].netReceivedBytes, 100U);

    std::vector older{process(10)}; // scanned before process 20 existed
    accumulator.publishTotals(older);
    EXPECT_EQ(older[0].netReceivedBytes, 0U);

    read(accumulator, {{.key = 1, .pid = 20, .bytesReceived = 110}}, newer);
    EXPECT_EQ(newer[1].netReceivedBytes, 110U) << "the older list must not have pruned process 20";
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

TEST(SocketTrafficAccumulatorTest, GrowthBeforeAttributionIsCreditedToTheLaterOwner)
{
    // #1259: the inode-to-PID map is rebuilt every few seconds, so a new connection can be in a few
    // readings before it has an owner. The bytes it moved in them used to be lost; they are held and
    // credited once it is attributed. Its bytes at its first sighting are not (they may predate it).
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {}, processes);
    read(accumulator, {{.key = 2, .pid = 0, .bytesReceived = 1'000, .bytesSent = 10}}, processes); // first sighting
    read(accumulator, {{.key = 2, .pid = 0, .bytesReceived = 5'000, .bytesSent = 40}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 0U) << "nothing is credited while it has no owner";

    read(accumulator, {{.key = 2, .pid = 10, .bytesReceived = 6'000, .bytesSent = 50}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 5'000U) << "the held 4000 plus this interval's 1000";
    EXPECT_EQ(processes[0].netSentBytes, 40U);

    read(accumulator, {{.key = 2, .pid = 10, .bytesReceived = 6'500, .bytesSent = 50}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 5'500U) << "the held bytes are credited once";
}

TEST(SocketTrafficAccumulatorTest, AnUnreadableSampleKeepsTheHeldGrowth)
{
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {{.key = 2, .pid = 0, .bytesReceived = 100}}, processes);
    read(accumulator, {{.key = 2, .pid = 0, .bytesReceived = 600}}, processes);
    read(accumulator, {{.key = 2, .pid = 0, .readable = false}}, processes);
    read(accumulator, {{.key = 2, .pid = 10, .bytesReceived = 900}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 800U);
}

TEST(SocketTrafficAccumulatorTest, AReusedKeyDropsTheHeldGrowth)
{
    // A counter going backwards means another connection took the key: the old one's held bytes
    // are not the new one's.
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    read(accumulator, {{.key = 2, .pid = 0, .bytesReceived = 100}}, processes);
    read(accumulator, {{.key = 2, .pid = 0, .bytesReceived = 9'000}}, processes);
    read(accumulator, {{.key = 2, .pid = 0, .bytesReceived = 50}}, processes); // reused
    read(accumulator, {{.key = 2, .pid = 10, .bytesReceived = 80}}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 30U);
}

TEST(SocketTrafficAccumulatorTest, HeldGrowthExpiresForAConnectionThatStaysUnowned)
{
    // One still unowned well past a map rebuild belongs to a process we can't read; if it is ever
    // attributed it must not land all its unowned traffic in one interval.
    constexpr std::uint64_t MS = 1'000'000ULL;
    constexpr std::uint64_t HOLD_NS = static_cast<std::uint64_t>(Sampling::UNATTRIBUTED_SOCKET_HOLD_MS) * MS;
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    const auto at = [&](std::uint64_t timeNs, const std::vector<SocketTrafficSample>& sockets)
    {
        accumulator.addReading(sockets, timeNs);
        accumulator.publish(processes);
    };
    constexpr std::uint64_t T0 = 1'000 * MS;
    at(T0, {{.key = 2, .pid = 0, .bytesReceived = 100}});
    at(T0 + HOLD_NS, {{.key = 2, .pid = 0, .bytesReceived = 1'100}}); // still within the hold
    at(T0 + HOLD_NS + MS, {{.key = 2, .pid = 0, .bytesReceived = 50'000}});
    at(T0 + HOLD_NS + (2 * MS), {{.key = 2, .pid = 0, .bytesReceived = 60'000}});
    at(T0 + HOLD_NS + (3 * MS), {{.key = 2, .pid = 10, .bytesReceived = 60'700}});
    EXPECT_EQ(processes[0].netReceivedBytes, 700U) << "only the growth since the previous reading";

    // A later unowned run of the same connection holds again.
    at(T0 + HOLD_NS + (4 * MS), {{.key = 2, .pid = 0, .bytesReceived = 61'000}});
    at(T0 + HOLD_NS + (5 * MS), {{.key = 2, .pid = 10, .bytesReceived = 61'200}});
    EXPECT_EQ(processes[0].netReceivedBytes, 1'200U);
}

TEST(SocketTrafficAccumulatorTest, ApplyHoldsGrowthOnTheReadingClock)
{
    // apply() passes each reading's time, so a connection attributed within the hold is credited
    // what it moved since its first sighting.
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    constexpr std::uint64_t SECOND = 1'000'000'000ULL;
    accumulator.apply({.sockets = {}, .sampleTimeNs = 10 * SECOND}, processes);
    accumulator.apply({.sockets = {{.key = 2, .pid = 0, .bytesReceived = 100}}, .sampleTimeNs = 11 * SECOND}, processes);
    accumulator.apply({.sockets = {{.key = 2, .pid = 0, .bytesReceived = 400}}, .sampleTimeNs = 12 * SECOND}, processes);
    accumulator.apply({.sockets = {{.key = 2, .pid = 10, .bytesReceived = 500}}, .sampleTimeNs = 14 * SECOND}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 400U);
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

// apply(): how ProcessModel feeds each refresh's reading (#1261 review: the accounting is Domain's).

TEST(SocketTrafficAccumulatorTest, ApplyStampsEveryProcessWithTheReadingTime)
{
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10), process(20)};
    accumulator.apply({.sockets = {{.key = 1, .pid = 10}}, .sampleTimeNs = 5'000}, processes);
    EXPECT_EQ(processes[0].netSampleTimeNs, 5'000U);
    EXPECT_EQ(processes[1].netSampleTimeNs, 5'000U) << "processes without sockets too";
}

TEST(SocketTrafficAccumulatorTest, ApplyLeavesProcessesUntouchedUntilAReading)
{
    // A probe that never returns readings (Windows today, mocks) keeps its own counters.
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    processes[0].netReceivedBytes = 1'234;
    processes[0].netSampleTimeNs = 77;
    accumulator.apply({}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 1'234U);
    EXPECT_EQ(processes[0].netSampleTimeNs, 77U);
}

TEST(SocketTrafficAccumulatorTest, ApplyFoldsARepeatedReadingOnce)
{
    // The probe caches its query: the same reading (same time) comes back for several refreshes.
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    accumulator.apply({.sockets = {{.key = 1, .pid = 10, .bytesReceived = 0}}, .sampleTimeNs = 1'000}, processes);
    const SocketTrafficReading second{.sockets = {{.key = 1, .pid = 10, .bytesReceived = 500}}, .sampleTimeNs = 2'000};
    accumulator.apply(second, processes);
    accumulator.apply(second, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 500U);
    EXPECT_EQ(processes[0].netSampleTimeNs, 2'000U);
}

TEST(SocketTrafficAccumulatorTest, ApplyDoesNotFoldAnOlderReading)
{
    // Folding a reading older than the last one would rewind the baselines: socket 1 back at 100
    // and then at 1100 again would count those 1000 bytes twice.
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    accumulator.apply({.sockets = {{.key = 1, .pid = 10, .bytesReceived = 100}}, .sampleTimeNs = 1'000}, processes);
    accumulator.apply({.sockets = {{.key = 1, .pid = 10, .bytesReceived = 1'100}}, .sampleTimeNs = 3'000}, processes);
    ASSERT_EQ(processes[0].netReceivedBytes, 1'000U);

    accumulator.apply({.sockets = {{.key = 1, .pid = 10, .bytesReceived = 100}}, .sampleTimeNs = 2'000}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 1'000U);
    EXPECT_EQ(processes[0].netSampleTimeNs, 3'000U) << "the time of the reading the totals come from";

    accumulator.apply({.sockets = {{.key = 1, .pid = 10, .bytesReceived = 1'200}}, .sampleTimeNs = 4'000}, processes);
    EXPECT_EQ(processes[0].netReceivedBytes, 1'100U);
}

TEST(SocketTrafficAccumulatorTest, ApplyRepublishesTheLastTotalsForAFailedReading)
{
    // A failed reading (time 0) is neither folded -- its missing sockets would come back as new and
    // credit their lifetime bytes -- nor a reason to drop the totals: the last ones and their time
    // are republished, so ProcessModel holds the last rate.
    SocketTrafficAccumulator accumulator;
    std::vector processes{process(10)};
    accumulator.apply({.sockets = {{.key = 1, .pid = 10, .bytesReceived = 0}}, .sampleTimeNs = 1'000}, processes);
    accumulator.apply({.sockets = {{.key = 1, .pid = 10, .bytesReceived = 400}}, .sampleTimeNs = 2'000}, processes);

    std::vector again{process(10)};
    accumulator.apply({}, again);
    EXPECT_EQ(again[0].netReceivedBytes, 400U);
    EXPECT_EQ(again[0].netSampleTimeNs, 2'000U);

    accumulator.apply({.sockets = {{.key = 1, .pid = 10, .bytesReceived = 450}}, .sampleTimeNs = 3'000}, again);
    EXPECT_EQ(again[0].netReceivedBytes, 450U) << "only socket 1's growth, not its lifetime bytes";
}

TEST(SocketTrafficAccumulatorTest, OnlyAFoldingApplyPrunesExitedProcesses)
{
    // A refresh that doesn't fold a reading only reads the totals; a process missing from its list
    // keeps its totals for the next refresh that does.
    SocketTrafficAccumulator accumulator;
    std::vector both{process(10), process(20)};
    accumulator.apply({.sockets = {{.key = 1, .pid = 20, .bytesReceived = 0}}, .sampleTimeNs = 1'000}, both);
    accumulator.apply({.sockets = {{.key = 1, .pid = 20, .bytesReceived = 100}}, .sampleTimeNs = 2'000}, both);
    ASSERT_EQ(both[1].netReceivedBytes, 100U);

    std::vector withoutTwenty{process(10)};
    accumulator.apply({.sockets = {{.key = 1, .pid = 20, .bytesReceived = 100}}, .sampleTimeNs = 2'000}, withoutTwenty);

    accumulator.apply({.sockets = {{.key = 1, .pid = 20, .bytesReceived = 110}}, .sampleTimeNs = 3'000}, both);
    EXPECT_EQ(both[1].netReceivedBytes, 110U);
}

} // namespace
} // namespace Domain
