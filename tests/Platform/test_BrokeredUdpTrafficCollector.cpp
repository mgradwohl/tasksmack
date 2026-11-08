// Linux's UDP collector through the privileged helper (#3366): it counts only within an Administrator session,
// reads the helper's totals once per sample, and keeps the last ones when the helper is busy. Shared code (the
// broker interface and the protocol), so it builds and runs on Windows too.

#include "Mocks/FakeBroker.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokeredUdpTrafficCollector.h"
#include "Platform/IUdpTrafficCollector.h"

#include <gtest/gtest.h>

#include <expected>
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

void answerWith(FakeBroker& broker, Broker::UdpBytesReply reply)
{
    broker.setAnswer([reply](const Broker::Request&) -> std::expected<Broker::Response, std::string> { return Broker::Response{reply}; });
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

    // The user chooses Administrator: the session's helper counts, and the next reading has its totals.
    broker->setHoldsSessions(true);
    ASSERT_TRUE(broker->holdForSession(std::stop_token{}).has_value());
    answerWith(*broker, {.status = Broker::UdpBytesStatus::Ok, .detail = {}, .processes = {{.tgid = 3, .txBytes = 4, .rxBytes = 5}}});
    ASSERT_EQ(collector.sample().size(), 1U);
    EXPECT_TRUE(collector.isDelivering());
    EXPECT_TRUE(collector.problem().empty());
}

TEST(BrokeredUdpTrafficCollectorTest, InAHeldSessionItReadsTheHelpersTotalsAsTheProcessesUdpBytes)
{
    auto broker = heldBroker();
    answerWith(*broker,
               {.status = Broker::UdpBytesStatus::Ok,
                .detail = {},
                .processes = {{.tgid = 7, .txBytes = 100, .rxBytes = 50}, {.tgid = 9, .txBytes = 1, .rxBytes = 2}}});
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    EXPECT_TRUE(broker->countingUdp());
    EXPECT_EQ(collector.start(), UdpCollectorStartResult::Started) << "already running";

    const std::vector<UdpProcessTraffic> traffic = collector.sample();
    ASSERT_EQ(traffic.size(), 2U);
    EXPECT_EQ(traffic[0].pid, 7U);
    EXPECT_EQ(traffic[0].bytesSent, 100U);
    EXPECT_EQ(traffic[0].bytesReceived, 50U);
    EXPECT_TRUE(collector.isDelivering());
    EXPECT_TRUE(collector.problem().empty());
    EXPECT_EQ(broker->idleCallCount(), 1) << "the sampler's read never waits on another call";
    ASSERT_EQ(broker->requests().size(), 1U);
    EXPECT_TRUE(std::holds_alternative<Broker::UdpBytesRequest>(broker->requests()[0]));
}

TEST(BrokeredUdpTrafficCollectorTest, ABusyHelperLeavesTheLastTotalsStanding)
{
    auto broker = heldBroker();
    answerWith(*broker, {.status = Broker::UdpBytesStatus::Ok, .detail = {}, .processes = {{.tgid = 7, .txBytes = 100, .rxBytes = 50}}});
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    ASSERT_EQ(collector.sample().size(), 1U);

    broker->setAnswer([](const Broker::Request&) -> std::expected<Broker::Response, std::string> { return std::unexpected("busy"); });
    const std::vector<UdpProcessTraffic> traffic = collector.sample();
    ASSERT_EQ(traffic.size(), 1U);
    EXPECT_EQ(traffic[0].bytesSent, 100U);
    EXPECT_TRUE(collector.isDelivering());
}

TEST(BrokeredUdpTrafficCollectorTest, AHelperThatIsntCountingDeliversNothingAndSaysWhy)
{
    auto broker = heldBroker();
    answerWith(*broker, {.status = Broker::UdpBytesStatus::Failed, .detail = "fexit: loading failed: no BTF", .processes = {}});
    BrokeredUdpTrafficCollector collector(broker);
    ASSERT_EQ(collector.start(), UdpCollectorStartResult::Started);
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_FALSE(collector.isDelivering());
    EXPECT_EQ(collector.problem(), "fexit: loading failed: no BTF");

    answerWith(*broker, {.status = Broker::UdpBytesStatus::NotCounting, .detail = {}, .processes = {}});
    EXPECT_TRUE(collector.sample().empty());
    EXPECT_EQ(collector.problem(), "The helper isn't counting UDP");
}

TEST(BrokeredUdpTrafficCollectorTest, StoppingForgetsTheTotalsAndTheNextHelperDoesntCount)
{
    auto broker = heldBroker();
    answerWith(*broker, {.status = Broker::UdpBytesStatus::Ok, .detail = {}, .processes = {{.tgid = 7, .txBytes = 1, .rxBytes = 1}}});
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
