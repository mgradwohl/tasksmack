// Linux's UDP collector through the privileged helper (#3366): it counts only within an Administrator session,
// reads the helper's totals once per sample past each run's baseline, and keeps the last ones when the helper is
// busy. Shared code (the broker interface and the protocol), so it builds and runs on Windows too.

#include "Mocks/FakeBroker.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokeredUdpTrafficCollector.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/UdpByteCounts.h"

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

/// Answers each ReadUdpBytes with one process's growing totals: sent @p sent, received @p received.
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

[[nodiscard]] const UdpProcessTraffic* find(const std::vector<UdpProcessTraffic>& traffic, std::uint32_t pid)
{
    for (const UdpProcessTraffic& process : traffic)
    {
        if (process.pid == pid)
        {
            return &process;
        }
    }
    return nullptr;
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

    // The user chooses Administrator: the session's helper counts, and readings deliver from its first one.
    broker->setHoldsSessions(true);
    ASSERT_TRUE(broker->holdForSession(std::stop_token{}).has_value());
    answerWith(*broker, {{.tgid = 3, .txBytes = 4, .rxBytes = 5}});
    ASSERT_EQ(collector.sample().size(), 1U);
    EXPECT_TRUE(collector.isDelivering());
    EXPECT_TRUE(collector.problem().empty());
}

TEST(BrokeredUdpTrafficCollectorTest, InAHeldSessionItReadsWhatTheHelperCountsAfterTheRunsFirstReading)
{
    auto broker = heldBroker();
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    EXPECT_TRUE(broker->countingUdp());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Started) << "already running";

    answerWith(*broker, {{.tgid = 7, .txBytes = 100, .rxBytes = 50}});
    const std::vector<UdpProcessTraffic> baseline = collector.sample();
    ASSERT_EQ(baseline.size(), 1U);
    EXPECT_EQ(baseline[0].bytesSent, 0U) << "the run's first totals are its baseline";
    EXPECT_TRUE(collector.isDelivering());

    answerWith(*broker, {{.tgid = 7, .txBytes = 160, .rxBytes = 80}, {.tgid = 9, .txBytes = 1, .rxBytes = 2}});
    const std::vector<UdpProcessTraffic> traffic = collector.sample();
    ASSERT_EQ(traffic.size(), 2U);
    ASSERT_NE(find(traffic, 7), nullptr);
    EXPECT_EQ(find(traffic, 7)->bytesSent, 60U);
    EXPECT_EQ(find(traffic, 7)->bytesReceived, 30U);
    ASSERT_NE(find(traffic, 9), nullptr);
    EXPECT_EQ(find(traffic, 9)->bytesSent, 1U) << "a process first seen after the baseline counts in full";
    EXPECT_EQ(broker->idleCallCount(), 2) << "the sampler's read never waits on another call";
}

// Turned off and on again with the same helper still counting: what it counted meanwhile, and before, isn't counted
// again as new connections.
TEST(BrokeredUdpTrafficCollectorTest, EnableDisableEnableCountsOnlyWhatComesAfterEachStart)
{
    auto broker = heldBroker();
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    answerWith(*broker, {{.tgid = 7, .txBytes = 100, .rxBytes = 100}});
    (void) collector.sample();
    answerWith(*broker, {{.tgid = 7, .txBytes = 150, .rxBytes = 100}});
    ASSERT_EQ(find(collector.sample(), 7)->bytesSent, 50U);

    collector.stop();
    EXPECT_TRUE(collector.sample().empty());
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    answerWith(*broker, {{.tgid = 7, .txBytes = 900, .rxBytes = 700}}); // what the helper counted while it was off
    EXPECT_EQ(find(collector.sample(), 7)->bytesSent, 0U);
    answerWith(*broker, {{.tgid = 7, .txBytes = 910, .rxBytes = 700}});
    EXPECT_EQ(find(collector.sample(), 7)->bytesSent, 10U);
}

TEST(BrokeredUdpTrafficCollectorTest, ANewHelpersCounterBelowTheBaselineIsTakenFromZero)
{
    auto broker = heldBroker();
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    answerWith(*broker, {{.tgid = 7, .txBytes = 1000, .rxBytes = 1000}});
    (void) collector.sample();
    answerWith(*broker, {{.tgid = 7, .txBytes = 30, .rxBytes = 20}}); // a new helper, counting from zero
    const std::vector<UdpProcessTraffic> traffic = collector.sample();
    const UdpProcessTraffic* process = find(traffic, 7);
    ASSERT_NE(process, nullptr);
    EXPECT_EQ(process->bytesSent, 30U);
    EXPECT_EQ(process->bytesReceived, 20U);
}

TEST(BrokeredUdpTrafficCollectorTest, ABusyHelperLeavesTheLastTotalsStandingAndAnEndedOneStopsDelivering)
{
    auto broker = heldBroker();
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    answerWith(*broker, {{.tgid = 7, .txBytes = 100, .rxBytes = 50}});
    (void) collector.sample();
    answerWith(*broker, {{.tgid = 7, .txBytes = 140, .rxBytes = 50}});
    ASSERT_EQ(find(collector.sample(), 7)->bytesSent, 40U);

    broker->setAnswer([](const Broker::Request&) -> std::expected<Broker::Response, std::string> { return std::unexpected("busy"); });
    const std::vector<UdpProcessTraffic> busy = collector.sample();
    ASSERT_EQ(busy.size(), 1U);
    EXPECT_EQ(busy[0].bytesSent, 40U);
    EXPECT_TRUE(collector.isDelivering());

    broker->setConnected(false); // the helper exited
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_FALSE(collector.isDelivering()) << "frozen totals aren't UDP counters";
    EXPECT_EQ(collector.problem(), "The helper ended");
}

TEST(BrokeredUdpTrafficCollectorTest, AHelperThatIsntCountingDeliversNothingAndSaysWhy)
{
    auto broker = heldBroker();
    answerStatus(*broker, Broker::UdpBytesStatus::Failed, "fexit: loading failed: no BTF");
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_FALSE(collector.isDelivering());
    EXPECT_EQ(collector.problem(), "fexit: loading failed: no BTF");

    answerStatus(*broker, Broker::UdpBytesStatus::NotCounting, {});
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_EQ(collector.problem(), "The helper isn't counting UDP");
}

// The session's helper couldn't be restarted with the counter (a dismissed prompt): that's a failure, not a missing
// session, though the broker is left disconnected; and the next start() tries again.
TEST(BrokeredUdpTrafficCollectorTest, AFailedRestartInAHeldSessionIsAFailureAndTheNextStartRetries)
{
    auto broker = heldBroker();
    broker->setCountUdpFailure("the prompt was dismissed");
    BrokeredUdpTrafficCollector collector(broker);
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Failed);
    EXPECT_EQ(collector.problem(), "the prompt was dismissed");
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_TRUE(broker->requests().empty()) << "stopped: nothing is read";

    broker->setCountUdpFailure({});
    broker->setConnected(true);
    ASSERT_TRUE(broker->holdForSession(std::stop_token{}).has_value());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Started) << "tried again, not taken for running";
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
