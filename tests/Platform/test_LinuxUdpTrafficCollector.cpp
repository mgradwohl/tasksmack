// Root's in-process UDP collector (#3580): what it reports to Domain from a counter's readings, with the counter faked.
// The real counter's load and read as root are UdpByteCounterRootTest's (test_UdpByteCounter.cpp).

#include "Platform/IUdpTrafficCollector.h"
#include "Platform/Linux/LinuxUdpTrafficCollector.h"
#include "Platform/UdpByteCounts.h"

#include <gtest/gtest.h>

#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Platform
{
namespace
{

/// A counter the test drives: each read returns what `next` holds then, and counts how many loads and unloads ran.
struct FakeCounter
{
    std::expected<std::vector<UdpByteCounts>, std::string> next = std::vector<UdpByteCounts>{};
    std::string loadError; ///< Non-empty: the load fails with it
    int loads = 0;
    std::shared_ptr<int> alive = std::make_shared<int>(0); ///< One use per live loaded counter, plus this one

    [[nodiscard]] LinuxUdpTrafficCollector::Load load()
    {
        return [this]() -> std::expected<LinuxUdpTrafficCollector::Read, std::string>
        {
            if (!loadError.empty())
            {
                return std::unexpected(loadError);
            }
            ++loads;
            return LinuxUdpTrafficCollector::Read{[this, held = alive] { return next; }};
        };
    }
};

TEST(LinuxUdpTrafficCollectorTest, IsEmptyAndNotDeliveringBeforeStart)
{
    FakeCounter counter;
    LinuxUdpTrafficCollector collector(counter.load());
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_FALSE(collector.isDelivering());
    EXPECT_FALSE(collector.unsupported());
    EXPECT_TRUE(collector.problem().empty());
    EXPECT_EQ(counter.loads, 0);
}

TEST(LinuxUdpTrafficCollectorTest, ReportsEachProcessesTotalsAsAllNewBytesFromItsFirstReading)
{
    FakeCounter counter;
    counter.next = std::vector<UdpByteCounts>{{.tgid = 42, .txBytes = 100, .rxBytes = 200}, {.tgid = 7, .txBytes = 1, .rxBytes = 0}};
    LinuxUdpTrafficCollector collector(counter.load());
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);

    const std::vector<UdpProcessTraffic> traffic = collector.sample();
    ASSERT_EQ(traffic.size(), 2U);
    EXPECT_EQ(traffic[0].pid, 42U);
    EXPECT_EQ(traffic[0].bytesSent, 100U);
    EXPECT_EQ(traffic[0].bytesReceived, 200U);
    EXPECT_TRUE(traffic[0].readable);
    EXPECT_FALSE(traffic[0].baselineOnly) << "the counter loaded at start(): every byte in it is new";
    EXPECT_EQ(traffic[1].pid, 7U);
    EXPECT_TRUE(collector.isDelivering());
    EXPECT_TRUE(collector.problem().empty());
}

TEST(LinuxUdpTrafficCollectorTest, StartingWhileRunningLoadsNothingMore)
{
    FakeCounter counter;
    LinuxUdpTrafficCollector collector(counter.load());
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    EXPECT_EQ(counter.loads, 1);
}

TEST(LinuxUdpTrafficCollectorTest, ALoadThatFailsIsUnsupportedWithItsReasonUntilOneLoads)
{
    FakeCounter counter;
    counter.loadError = "no BTF";
    LinuxUdpTrafficCollector collector(counter.load());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Failed);
    EXPECT_TRUE(collector.unsupported());
    EXPECT_EQ(collector.problem(), "no BTF");
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_FALSE(collector.isDelivering());

    counter.loadError.clear();
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Started);
    EXPECT_FALSE(collector.unsupported());
    EXPECT_TRUE(collector.problem().empty());
}

TEST(LinuxUdpTrafficCollectorTest, AReadTheKernelRefusesIsEmptyAndTheNextOneReadOnlySetsTheBaseline)
{
    FakeCounter counter;
    counter.next = std::vector<UdpByteCounts>{{.tgid = 42, .txBytes = 100, .rxBytes = 0}};
    LinuxUdpTrafficCollector collector(counter.load());
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    ASSERT_EQ(collector.sample().size(), 1U);

    counter.next = std::unexpected(std::string("Operation not permitted"));
    EXPECT_TRUE(collector.sample().empty()) << "Domain takes the connections as closed";
    EXPECT_FALSE(collector.isDelivering());
    EXPECT_EQ(collector.problem(), "Operation not permitted");
    EXPECT_FALSE(collector.unsupported()) << "it loaded; a read can work again";

    counter.next = std::vector<UdpByteCounts>{{.tgid = 42, .txBytes = 500, .rxBytes = 0}};
    const std::vector<UdpProcessTraffic> after = collector.sample();
    ASSERT_EQ(after.size(), 1U);
    EXPECT_TRUE(after[0].baselineOnly) << "its totals include the interval that wasn't read";
    EXPECT_TRUE(collector.isDelivering());
    EXPECT_TRUE(collector.problem().empty());

    const std::vector<UdpProcessTraffic> next = collector.sample();
    ASSERT_EQ(next.size(), 1U);
    EXPECT_FALSE(next[0].baselineOnly);
}

TEST(LinuxUdpTrafficCollectorTest, StopUnloadsTheCounterAndAStartAfterItIsAnotherGeneration)
{
    FakeCounter counter;
    counter.next = std::vector<UdpByteCounts>{{.tgid = 42, .txBytes = 100, .rxBytes = 0}};
    LinuxUdpTrafficCollector collector(counter.load());
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    const std::vector<UdpProcessTraffic> first = collector.sample();
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(counter.alive.use_count(), 2) << "the loaded counter is held";

    collector.stop();
    EXPECT_EQ(counter.alive.use_count(), 1) << "stop() unloads it";
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_FALSE(collector.isDelivering());

    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    EXPECT_EQ(counter.loads, 2);
    const std::vector<UdpProcessTraffic> second = collector.sample();
    ASSERT_EQ(second.size(), 1U);
    EXPECT_NE(second[0].generation, first[0].generation) << "a new counter counts from zero: new connections to Domain";
    EXPECT_FALSE(second[0].baselineOnly);
}

} // namespace
} // namespace Platform
