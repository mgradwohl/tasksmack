/// @file test_ProcessRowHistory.cpp
/// @brief Per-process histories for the Processes table's sparklines (#1612): the ring, the tracker
/// that carries rings between generations and prunes them with it, and ProcessModel publishing them
/// with each generation only while a metric is asked for.

#include "Domain/ProcessModel.h"
#include "Domain/ProcessRowHistory.h"
#include "Domain/ProcessSnapshot.h"
#include "Mocks/MockProbes.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace
{

using Domain::ProcessRowHistory::bitOf;
using Domain::ProcessRowHistory::Generation;
using Domain::ProcessRowHistory::Metric;
using Domain::ProcessRowHistory::MetricSet;
using Domain::ProcessRowHistory::Ring;
using Domain::ProcessRowHistory::RING_CAPACITY;
using Domain::ProcessRowHistory::Tracker;

Domain::ProcessSnapshot makeProc(std::uint64_t key, double cpu, std::uint64_t memoryBytes = 0)
{
    Domain::ProcessSnapshot proc;
    proc.pid = static_cast<std::int32_t>(key);
    proc.uniqueKey = key;
    proc.cpuPercent = cpu;
    proc.memoryBytes = memoryBytes;
    return proc;
}

/// Expects @p ring to hold @p expected, oldest first.
void expectValues(const Ring& ring, const std::vector<float>& expected)
{
    ASSERT_EQ(ring.size(), expected.size());
    std::size_t i = 0;
    while (i < expected.size())
    {
        EXPECT_FLOAT_EQ(ring.at(i), expected[i]) << "value " << i;
        ++i;
    }
}

// ========== Ring ==========

TEST(ProcessRowHistoryRingTest, KeepsTheLatestValuesOldestFirst)
{
    Ring ring;
    EXPECT_EQ(ring.size(), 0U);
    EXPECT_FLOAT_EQ(ring.latest(), 0.0F);
    ring.push(1.0F);
    ring.push(2.0F);
    expectValues(ring, {1.0F, 2.0F});
    EXPECT_FLOAT_EQ(ring.latest(), 2.0F);
}

TEST(ProcessRowHistoryRingTest, DropsTheOldestOnceFull)
{
    Ring ring;
    std::size_t i = 0;
    while (i < RING_CAPACITY + 5)
    {
        ring.push(static_cast<float>(i));
        ++i;
    }
    ASSERT_EQ(ring.size(), RING_CAPACITY);
    EXPECT_FLOAT_EQ(ring.at(0), 5.0F);
    EXPECT_FLOAT_EQ(ring.latest(), static_cast<float>(RING_CAPACITY + 4));
}

TEST(ProcessRowHistoryRingTest, AValueThatIsNotFiniteIsKeptAsZero)
{
    Ring ring;
    ring.push(std::numeric_limits<float>::quiet_NaN());
    ring.push(std::numeric_limits<float>::infinity());
    expectValues(ring, {0.0F, 0.0F});
}

TEST(ProcessRowHistoryRingTest, IsAbout120Bytes)
{
    EXPECT_LE(sizeof(Ring), (RING_CAPACITY * sizeof(float)) + 4U);
}

// ========== Tracker ==========

TEST(ProcessRowHistoryTrackerTest, FillsEachProcessRingAcrossGenerations)
{
    Tracker tracker;
    const MetricSet cpu = bitOf(Metric::CpuPercent);
    (void) tracker.advance({makeProc(10, 1.0), makeProc(20, 5.0)}, cpu);
    // The second generation lists them in another order: rings follow the process, not the row.
    const auto gen = tracker.advance({makeProc(20, 6.0), makeProc(10, 2.0)}, cpu);
    ASSERT_NE(gen, nullptr);
    ASSERT_EQ(gen->rowCount(), 2U);
    ASSERT_NE(gen->ring(0, Metric::CpuPercent), nullptr);
    expectValues(*gen->ring(0, Metric::CpuPercent), {5.0F, 6.0F});
    expectValues(*gen->ring(1, Metric::CpuPercent), {1.0F, 2.0F});
    EXPECT_EQ(gen->ring(0, Metric::Resident), nullptr);   // Not asked for
    EXPECT_EQ(gen->ring(2, Metric::CpuPercent), nullptr); // No such row
}

TEST(ProcessRowHistoryTrackerTest, AnExitedProcessIsPrunedAndANewOneStartsEmpty)
{
    Tracker tracker;
    const MetricSet cpu = bitOf(Metric::CpuPercent);
    (void) tracker.advance({makeProc(10, 1.0), makeProc(20, 5.0)}, cpu);
    (void) tracker.advance({makeProc(10, 2.0)}, cpu); // 20 exited
    EXPECT_EQ(tracker.rowCount(), 1U);
    // A process listed again under the same key after a generation without it starts over.
    const auto gen = tracker.advance({makeProc(10, 3.0), makeProc(20, 7.0)}, cpu);
    expectValues(*gen->ring(0, Metric::CpuPercent), {1.0F, 2.0F, 3.0F});
    expectValues(*gen->ring(1, Metric::CpuPercent), {7.0F});
}

TEST(ProcessRowHistoryTrackerTest, APublishedGenerationNeverChanges)
{
    Tracker tracker;
    const MetricSet cpu = bitOf(Metric::CpuPercent);
    const auto first = tracker.advance({makeProc(10, 1.0)}, cpu);
    (void) tracker.advance({makeProc(10, 2.0)}, cpu);
    expectValues(*first->ring(0, Metric::CpuPercent), {1.0F});
}

TEST(ProcessRowHistoryTrackerTest, AddingAMetricKeepsTheOthersHistory)
{
    Tracker tracker;
    (void) tracker.advance({makeProc(10, 1.0, 100)}, bitOf(Metric::CpuPercent));
    const auto both = static_cast<MetricSet>(bitOf(Metric::CpuPercent) | bitOf(Metric::Resident));
    const auto gen = tracker.advance({makeProc(10, 2.0, 200)}, both);
    expectValues(*gen->ring(0, Metric::CpuPercent), {1.0F, 2.0F});
    expectValues(*gen->ring(0, Metric::Resident), {200.0F});
}

TEST(ProcessRowHistoryTrackerTest, NoMetricsDropsEverything)
{
    Tracker tracker;
    EXPECT_EQ(tracker.bytes(), 0U);
    (void) tracker.advance({makeProc(10, 1.0), makeProc(20, 2.0)}, bitOf(Metric::CpuPercent));
    EXPECT_GE(tracker.bytes(), 2U * sizeof(Ring));
    EXPECT_EQ(tracker.advance({makeProc(10, 1.0)}, 0), nullptr);
    EXPECT_EQ(tracker.rowCount(), 0U);
    EXPECT_EQ(tracker.bytes(), 0U);
    // Turned on again, the history starts over.
    const auto gen = tracker.advance({makeProc(10, 4.0)}, bitOf(Metric::CpuPercent));
    expectValues(*gen->ring(0, Metric::CpuPercent), {4.0F});
}

TEST(ProcessRowHistoryTrackerTest, AKeyListedTwiceCarriesOnItsFirstRowsHistory)
{
    // A uniqueKey hash collision: both rows carry on the history of the key's first row, and the
    // next generation finds the first of them again.
    Tracker tracker;
    const MetricSet cpu = bitOf(Metric::CpuPercent);
    (void) tracker.advance({makeProc(10, 1.0)}, cpu);
    auto gen = tracker.advance({makeProc(10, 2.0), makeProc(10, 9.0)}, cpu);
    expectValues(*gen->ring(0, Metric::CpuPercent), {1.0F, 2.0F});
    expectValues(*gen->ring(1, Metric::CpuPercent), {1.0F, 9.0F});
    gen = tracker.advance({makeProc(10, 3.0)}, cpu);
    expectValues(*gen->ring(0, Metric::CpuPercent), {1.0F, 2.0F, 3.0F});
}

TEST(ProcessRowHistoryTrackerTest, AnUnreadValueIsKeptAsZero)
{
    Domain::ProcessSnapshot proc = makeProc(10, 0.0);
    proc.ioReadBytesPerSec = 500.0;
    proc.ioAvailable = false;
    Tracker tracker;
    const auto gen = tracker.advance({proc}, bitOf(Metric::IoRead));
    expectValues(*gen->ring(0, Metric::IoRead), {0.0F});
}

// #1799: the NPU % and NPU Memory sparklines follow the process's NPU figures, never its GPU ones,
// alongside the GPU's; read with the GPU fields, so unread they are kept as zero too.
TEST(ProcessRowHistoryTrackerTest, TheNpuMetricsFollowTheNpuFigures)
{
    Domain::ProcessSnapshot proc = makeProc(10, 0.0);
    proc.gpuUtilPercent = 5.0;
    proc.gpuMemoryBytes = 100;
    proc.npuUtilPercent = 70.0;
    proc.npuMemoryBytes = 8192;
    const auto all = static_cast<MetricSet>(0U | bitOf(Metric::GpuPercent) | bitOf(Metric::NpuPercent) | bitOf(Metric::NpuMemory));
    Tracker tracker;
    auto gen = tracker.advance({proc}, all);
    expectValues(*gen->ring(0, Metric::NpuPercent), {70.0F});
    expectValues(*gen->ring(0, Metric::NpuMemory), {8192.0F});
    expectValues(*gen->ring(0, Metric::GpuPercent), {5.0F});

    proc.gpuFieldsRead = false;
    gen = tracker.advance({proc}, all);
    expectValues(*gen->ring(0, Metric::NpuPercent), {70.0F, 0.0F});
    expectValues(*gen->ring(0, Metric::NpuMemory), {8192.0F, 0.0F});
}

// ========== ProcessModel ==========

using TestMocks::makeProcessCounters;
using TestMocks::MockProcessProbe;

struct Adopted
{
    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> snapshots;
    std::shared_ptr<const Generation> history;
    std::uint64_t version = 0;
};

Adopted adopt(const Domain::ProcessModel& model)
{
    Adopted adopted;
    EXPECT_TRUE(model.tryCopySnapshotsIfNewer(
        std::numeric_limits<std::uint64_t>::max(), adopted.snapshots, adopted.version, nullptr, nullptr, &adopted.history));
    return adopted;
}

TEST(ProcessModelRowHistoryTest, NoHistoryIsKeptByDefault)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeProcessCounters(100, "a"), makeProcessCounters(200, "b")});
    Domain::ProcessModel model(std::move(probe));
    EXPECT_EQ(model.rowHistoryMetrics(), 0U);
    model.refresh();
    model.refresh();
    const Adopted adopted = adopt(model);
    EXPECT_EQ(adopted.snapshots->size(), 2U);
    EXPECT_EQ(adopted.history, nullptr);
}

TEST(ProcessModelRowHistoryTest, EachGenerationPublishesRowAlignedRings)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->setCounters({makeProcessCounters(100, "a", 'R', 0, 0, 1000, 1000), makeProcessCounters(200, "b", 'R', 0, 0, 1000, 2000)});
    Domain::ProcessModel model(std::move(probe));
    model.setRowHistoryMetrics(bitOf(Metric::Resident));
    model.refresh();
    rawProbe->setCounters({makeProcessCounters(200, "b", 'R', 0, 0, 1000, 2500), makeProcessCounters(100, "a", 'R', 0, 0, 1000, 1500)});
    model.refresh();

    const Adopted adopted = adopt(model);
    ASSERT_NE(adopted.history, nullptr);
    ASSERT_EQ(adopted.history->rowCount(), adopted.snapshots->size());
    std::size_t row = 0;
    while (row < adopted.snapshots->size())
    {
        const Ring* ring = adopted.history->ring(row, Metric::Resident);
        ASSERT_NE(ring, nullptr);
        ASSERT_EQ(ring->size(), 2U);
        // Each row's ring is its own process's, ending with this generation's value.
        EXPECT_FLOAT_EQ(ring->latest(), static_cast<float>((*adopted.snapshots)[row].memoryBytes));
        EXPECT_FLOAT_EQ(ring->at(0), (*adopted.snapshots)[row].pid == 100 ? 1000.0F : 2000.0F);
        ++row;
    }
}

TEST(ProcessModelRowHistoryTest, AnExitedProcessHistoryIsPruned)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->setCounters({makeProcessCounters(100, "a"), makeProcessCounters(200, "b")});
    Domain::ProcessModel model(std::move(probe));
    model.setRowHistoryMetrics(bitOf(Metric::CpuPercent));
    model.refresh();
    rawProbe->setCounters({makeProcessCounters(100, "a")});
    model.refresh();
    const Adopted adopted = adopt(model);
    ASSERT_NE(adopted.history, nullptr);
    EXPECT_EQ(adopted.history->rowCount(), 1U);
    EXPECT_EQ(adopted.history->ring(0, Metric::CpuPercent)->size(), 2U);
}

TEST(ProcessModelRowHistoryTest, TurningItOffDropsTheHistoryFromTheNextGeneration)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeProcessCounters(100, "a")});
    Domain::ProcessModel model(std::move(probe));
    model.setRowHistoryMetrics(bitOf(Metric::CpuPercent));
    model.refresh();
    ASSERT_NE(adopt(model).history, nullptr);

    model.setRowHistoryMetrics(0);
    EXPECT_EQ(model.rowHistoryMetrics(), 0U);
    model.refresh();
    EXPECT_EQ(adopt(model).history, nullptr);

    // Back on: a fresh history, one value long.
    model.setRowHistoryMetrics(bitOf(Metric::CpuPercent));
    model.refresh();
    const Adopted adopted = adopt(model);
    ASSERT_NE(adopted.history, nullptr);
    EXPECT_EQ(adopted.history->ring(0, Metric::CpuPercent)->size(), 1U);
}

TEST(ProcessModelRowHistoryTest, ACallerThatDoesNotAskIsUnaffected)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeProcessCounters(100, "a")});
    Domain::ProcessModel model(std::move(probe));
    model.setRowHistoryMetrics(bitOf(Metric::CpuPercent));
    model.refresh();
    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> snapshots;
    std::uint64_t version = 0;
    EXPECT_TRUE(model.tryCopySnapshotsIfNewer(0, snapshots, version));
    EXPECT_EQ(snapshots->size(), 1U);
}

} // namespace
