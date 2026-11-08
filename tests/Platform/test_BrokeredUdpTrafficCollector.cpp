// Linux's UDP collector through the privileged helper (#3366): it counts only within an Administrator session and
// passes the helper's cumulative totals to Domain's SocketTrafficAccumulator, marking which reading is only a baseline
// (a run's first, a busy helper's repeat) and which counter is another (a new run or helper). Shared code (the broker
// interface, the protocol and Domain), so it builds and runs on Windows too.

#include "Domain/SocketTrafficAccumulator.h"
#include "Mocks/FakeBroker.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokeredUdpTrafficCollector.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/ProcessTypes.h"
#include "Platform/UdpByteCounts.h"
#include "Platform/UdpTrafficSamples.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

using Mocks::FakeBroker;

[[nodiscard]] std::shared_ptr<FakeBroker> heldBroker()
{
    auto broker = std::make_shared<FakeBroker>();
    broker->setHoldsSessions(true);
    EXPECT_TRUE(broker->holdForSession(std::stop_token{}).has_value());
    return broker;
}

/// Answers each ReadUdpBytes with @p processes' totals.
void answerWith(FakeBroker& broker, std::vector<UdpByteCounts> processes)
{
    broker.setAnswer(
        [processes](const Broker::Request&) -> std::expected<Broker::Response, std::string>
        { return Broker::Response{Broker::UdpBytesReply{.status = Broker::UdpBytesStatus::Ok, .detail = {}, .processes = processes}}; });
}

void answerStatus(FakeBroker& broker, Broker::UdpBytesStatus status, std::string detail)
{
    broker.setAnswer([status, detail](const Broker::Request&) -> std::expected<Broker::Response, std::string>
                     { return Broker::Response{Broker::UdpBytesReply{.status = status, .detail = detail, .processes = {}}}; });
}

void answerBusy(FakeBroker& broker)
{
    broker.setAnswer([](const Broker::Request&) -> std::expected<Broker::Response, std::string> { return std::unexpected("busy"); });
}

[[nodiscard]] UdpProcessTraffic only(const std::vector<UdpProcessTraffic>& traffic)
{
    EXPECT_EQ(traffic.size(), 1U);
    return traffic.empty() ? UdpProcessTraffic{} : traffic.front();
}

TEST(BrokeredUdpTrafficCollectorTest, WithoutAnAdministratorSessionItPromptsForNothingAndCountsOnceOneStarts)
{
    auto broker = std::make_shared<FakeBroker>(false);
    BrokeredUdpTrafficCollector collector(broker);
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::NotElevated);
    EXPECT_EQ(broker->connectCount(), 0) << "no helper started, no prompt";
    EXPECT_EQ(collector.problem(), "Needs an Administrator session");
    EXPECT_TRUE(broker->countingUdp()) << "the request stands for the session's helper";
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_FALSE(collector.isDelivering());
    EXPECT_EQ(collector.problem(), "Needs an Administrator session") << "no helper ever ran, so none ended";

    // The user chooses Administrator: the session's helper is a new one, counting from zero, so its totals are new.
    broker->setHoldsSessions(true);
    ASSERT_TRUE(broker->holdForSession(std::stop_token{}).has_value());
    answerWith(*broker, {{.tgid = 3, .txBytes = 4, .rxBytes = 5}});
    const UdpProcessTraffic first = only(collector.sample());
    EXPECT_TRUE(first.readable);
    EXPECT_EQ(first.bytesSent, 4U);
    EXPECT_TRUE(collector.isDelivering());
    EXPECT_TRUE(collector.problem().empty());
}

TEST(BrokeredUdpTrafficCollectorTest, ItPassesTheHelpersTotalsOnAndTheRunsFirstReadingOnlySeedsTheBaseline)
{
    auto broker = heldBroker();
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    EXPECT_TRUE(broker->countingUdp());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Started) << "already running";

    answerWith(*broker, {{.tgid = 7, .txBytes = 100, .rxBytes = 50}});
    const UdpProcessTraffic seed = only(collector.sample());
    EXPECT_TRUE(seed.readable);
    EXPECT_TRUE(seed.baselineOnly) << "the helper may have counted these before this run";
    EXPECT_EQ(seed.bytesSent, 100U) << "the helper's own totals";
    EXPECT_TRUE(collector.isDelivering());

    answerWith(*broker, {{.tgid = 7, .txBytes = 160, .rxBytes = 80}});
    const UdpProcessTraffic next = only(collector.sample());
    EXPECT_TRUE(next.readable);
    EXPECT_FALSE(next.baselineOnly);
    EXPECT_EQ(next.bytesSent, 160U);
    EXPECT_EQ(next.bytesReceived, 80U);
    EXPECT_EQ(next.generation, seed.generation);
    EXPECT_EQ(broker->idleCallCount(), 2) << "the sampler's read never waits on another call";
}

TEST(BrokeredUdpTrafficCollectorTest, EachRunAndEachNewHelperIsAnotherGeneration)
{
    auto broker = heldBroker();
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    answerWith(*broker, {{.tgid = 7, .txBytes = 100, .rxBytes = 0}});
    const std::uint32_t firstRun = only(collector.sample()).generation;

    collector.stop();
    EXPECT_TRUE(collector.sample().empty());
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    answerWith(*broker, {{.tgid = 7, .txBytes = 900, .rxBytes = 0}}); // the same helper counted on while this was off
    const UdpProcessTraffic resumed = only(collector.sample());
    EXPECT_NE(resumed.generation, firstRun) << "another connection to Domain";
    EXPECT_TRUE(resumed.baselineOnly) << "only a baseline: 900 includes bytes from before";

    // Another reader restarts the helper between two samples: its counter starts from zero, and it's new.
    broker->setConnected(false);
    ASSERT_TRUE(broker->connect(std::stop_token{}).has_value());
    answerWith(*broker, {{.tgid = 7, .txBytes = 150, .rxBytes = 0}});
    const UdpProcessTraffic replaced = only(collector.sample());
    EXPECT_NE(replaced.generation, resumed.generation);
    EXPECT_TRUE(replaced.readable) << "all of a new counter's bytes are new";
    EXPECT_FALSE(replaced.baselineOnly);
    EXPECT_EQ(replaced.bytesSent, 150U);
}

TEST(BrokeredUdpTrafficCollectorTest, ABusyHelperSendsTheLastTotalsUnreadableAndAnEndedOneStopsDelivering)
{
    auto broker = heldBroker();
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    answerWith(*broker, {{.tgid = 7, .txBytes = 100, .rxBytes = 50}});
    (void) collector.sample();
    answerWith(*broker, {{.tgid = 7, .txBytes = 140, .rxBytes = 50}});
    (void) collector.sample();

    answerBusy(*broker);
    const UdpProcessTraffic busy = only(collector.sample());
    EXPECT_EQ(busy.bytesSent, 140U);
    EXPECT_FALSE(busy.readable) << "kept open, re-baselined next time";
    EXPECT_TRUE(collector.isDelivering());

    broker->setConnected(false); // the helper exited
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_FALSE(collector.isDelivering()) << "frozen totals aren't UDP counters";
    EXPECT_EQ(collector.problem(), "The helper ended");
}

TEST(BrokeredUdpTrafficCollectorTest, AFailedReadIsFollowedByASeedingReading)
{
    auto broker = heldBroker();
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    answerWith(*broker, {{.tgid = 7, .txBytes = 100, .rxBytes = 0}});
    (void) collector.sample();
    answerWith(*broker, {{.tgid = 7, .txBytes = 200, .rxBytes = 0}});
    EXPECT_TRUE(only(collector.sample()).readable);

    answerStatus(*broker, Broker::UdpBytesStatus::Failed, "the kernel refused reading the UDP byte counter");
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_EQ(collector.problem(), "the kernel refused reading the UDP byte counter");
    answerWith(*broker, {{.tgid = 7, .txBytes = 300, .rxBytes = 0}});
    EXPECT_TRUE(only(collector.sample()).baselineOnly) << "the same helper kept counting: a baseline, not new bytes";

    answerStatus(*broker, Broker::UdpBytesStatus::NotCounting, {});
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_EQ(collector.problem(), "The helper isn't counting UDP");
}

// The session's helper couldn't be restarted with the counter (a dismissed prompt): a failure, not a missing session,
// though the broker is left disconnected. It stays armed, so the session's next helper counts without another start(),
// and a later start() asks again rather than reporting it runs.
TEST(BrokeredUdpTrafficCollectorTest, AFailedRestartInAHeldSessionIsAFailureThatStaysArmed)
{
    auto broker = heldBroker();
    broker->setCountUdpFailure("the prompt was dismissed");
    BrokeredUdpTrafficCollector collector(broker);
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Failed);
    EXPECT_EQ(collector.problem(), "the prompt was dismissed");
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::NotElevated) << "asked again: no session now, not \"Started\"";

    // The user chooses Administrator again: the session's helper counts, and the armed collector delivers.
    broker->setCountUdpFailure({});
    broker->setConnected(true);
    ASSERT_TRUE(broker->holdForSession(std::stop_token{}).has_value());
    answerWith(*broker, {{.tgid = 7, .txBytes = 9, .rxBytes = 0}});
    EXPECT_FALSE(collector.sample().empty());
    EXPECT_TRUE(collector.isDelivering());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Started) << "counting: asked once, answered at once";
}

// At launch a held helper may have exited without anyone noticing yet: that isn't a session, so start() reports the
// Administrator requirement, not a failed restart, and stays armed for the next one.
TEST(BrokeredUdpTrafficCollectorTest, AHeldHelperThatExitedUnnoticedIsNoSession)
{
    auto broker = heldBroker();
    broker->setConnected(false); // gone, and only connected() would find out
    BrokeredUdpTrafficCollector collector(broker);
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::NotElevated);
    EXPECT_EQ(collector.problem(), "Needs an Administrator session");
    EXPECT_TRUE(broker->countingUdp());
}

// A read in flight when stop() runs publishes nothing: the totals stay cleared and delivery off.
TEST(BrokeredUdpTrafficCollectorTest, AReadInFlightAcrossStopPublishesNothing)
{
    auto broker = heldBroker();
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    std::function<void()> duringRead;
    broker->setAnswer(
        [&duringRead](const Broker::Request&) -> std::expected<Broker::Response, std::string>
        {
            if (duringRead)
            {
                duringRead();
            }
            return Broker::Response{Broker::UdpBytesReply{
                .status = Broker::UdpBytesStatus::Ok, .detail = {}, .processes = {{.tgid = 7, .txBytes = 5, .rxBytes = 5}}}};
        });
    duringRead = [&collector]
    {
        collector.stop();
    };
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_FALSE(collector.isDelivering());
    duringRead = nullptr;
    EXPECT_TRUE(collector.sample().empty()) << "still stopped";
}

TEST(BrokeredUdpTrafficCollectorTest, StoppingForgetsTheTotalsAndTheNextHelperDoesntCount)
{
    auto broker = heldBroker();
    answerWith(*broker, {{.tgid = 7, .txBytes = 1, .rxBytes = 1}});
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    ASSERT_FALSE(collector.sample().empty());
    collector.stop();
    EXPECT_FALSE(broker->countingUdp());
    EXPECT_FALSE(collector.isDelivering());
    EXPECT_TRUE(collector.sample().empty());
}

/// One refresh of Domain's accumulator with the collector's sample for process 7, at @p timeNs; its sent total after.
[[nodiscard]] std::uint64_t
foldSample(Domain::SocketTrafficAccumulator& accumulator, BrokeredUdpTrafficCollector& collector, std::uint64_t timeNs)
{
    SocketTrafficReading reading{.sockets = {}, .sampleTimeNs = timeNs};
    UdpTraffic::appendSamples(reading.sockets, collector.sample());
    ProcessCounters process;
    process.pid = 7;
    process.startTimeTicks = 1000;
    std::vector<ProcessCounters> processes{process};
    accumulator.apply(reading, processes);
    return processes.front().netSentBytes;
}

// A helper replaced between the reply and the collector's look at the broker: the reply's counter is unknown (the old
// helper's, or the new one's), so it's dropped, and the next reading is the new helper's generation, counted in full.
TEST(BrokeredUdpTrafficCollectorTest, AReplyAcrossAHelperReplacementIsDropped)
{
    auto broker = heldBroker();
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    answerWith(*broker, {{.tgid = 7, .txBytes = 5000, .rxBytes = 0}});
    const std::uint32_t before = only(collector.sample()).generation;

    broker->setAfterIdleCall(
        [&broker]
        {
            broker->setConnected(false);
            EXPECT_TRUE(broker->connect(std::stop_token{}).has_value()); // another worker restarts the helper
        });
    EXPECT_TRUE(collector.sample().empty()) << "whose counter is 5000? dropped";
    broker->setAfterIdleCall(nullptr);
    answerWith(*broker, {{.tgid = 7, .txBytes = 40, .rxBytes = 0}});
    const UdpProcessTraffic after = only(collector.sample());
    EXPECT_NE(after.generation, before);
    EXPECT_FALSE(after.baselineOnly);
    EXPECT_EQ(after.bytesSent, 40U);
}

// Through Domain: steady traffic of 100 bytes a reading, a run resumed on a helper that counted on meanwhile, a busy
// read and a new helper, with the exact totals each path credits.
TEST(BrokeredUdpTrafficCollectorTest, ThroughTheAccumulatorEachIntervalIsCreditedOnceAndNothingFromBeforeTheRun)
{
    auto broker = heldBroker();
    BrokeredUdpTrafficCollector collector(broker);
    Domain::SocketTrafficAccumulator accumulator;
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    std::uint64_t total = 0;
    std::uint64_t counter = 10'000; // counted before this run
    std::uint64_t timeNs = 1'000;
    const auto step = [&](bool busy)
    {
        counter += 100;
        if (busy)
        {
            answerBusy(*broker);
        }
        else
        {
            answerWith(*broker, {{.tgid = 7, .txBytes = counter, .rxBytes = 0}});
        }
        const std::uint64_t after = foldSample(accumulator, collector, timeNs += 1'000);
        EXPECT_LE(after - total, 100U) << "at " << timeNs;
        total = after;
    };
    step(false); // the run's baseline: 10,100, credited nothing
    EXPECT_EQ(total, 0U);
    for (int i = 0; i < 3; ++i)
    {
        step(false);
    }
    EXPECT_EQ(total, 300U) << "each interval after the baseline, once";
    step(true);  // the helper was busy: that interval is the last totals again, unreadable, credited nothing
    step(false); // read again after it: re-baselined, as its growth spans the busy one
    EXPECT_EQ(total, 300U) << "no burst after the busy read";
    for (int i = 0; i < 2; ++i)
    {
        step(false);
    }
    EXPECT_EQ(total, 500U);

    // A new helper, counting from zero: another generation, so its first 100 bytes are new.
    broker->setConnected(false);
    ASSERT_TRUE(broker->connect(std::stop_token{}).has_value());
    counter = 0;
    step(false);
    EXPECT_EQ(total, 600U);
}

TEST(BrokeredUdpTrafficCollectorTest, ThePlatformsOwnCollectorComesFirstAndNoBrokerMeansNone)
{
    auto broker = std::make_shared<FakeBroker>();
    auto own = std::make_shared<BrokeredUdpTrafficCollector>(broker); // stands in for Windows' ETW collector
    EXPECT_EQ(withPrivilegedBroker(std::shared_ptr<IUdpTrafficCollector>(own), broker), own);
    EXPECT_EQ(withPrivilegedBroker(std::shared_ptr<IUdpTrafficCollector>{}, nullptr), nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<BrokeredUdpTrafficCollector>(withPrivilegedBroker(std::shared_ptr<IUdpTrafficCollector>{}, broker)),
              nullptr);
}

} // namespace
} // namespace Platform
