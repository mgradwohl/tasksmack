/// @file test_BrokeredServiceActions.cpp
/// @brief Service actions with the privileged helper behind them (#2728): an action refused only for want
/// of authorization goes to a running helper, never prompting; Authenticate and retry starts the helper and
/// acts through it; a helper that doesn't do service actions leaves TaskSmack's own answer, and polkit's
/// prompt, as they were.

#include "Mocks/FakeBroker.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokeredServiceActions.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"

#include <gtest/gtest.h>

#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Platform
{
namespace
{

/// TaskSmack's own service actions: each answers @c answer and is recorded.
class RecordingServiceActions final : public IServiceActions
{
  public:
    ServiceActionResult answer = ServiceActionResult::succeeded();
    std::vector<std::string> calls;

    [[nodiscard]] ServiceActionCapabilities capabilities() const override
    {
        ServiceActionCapabilities caps;
        caps.canStart = true;
        caps.canStop = true;
        caps.canRestart = true;
        caps.canSetStartType = true;
        return caps;
    }
    [[nodiscard]] ServiceActionResult start(std::string_view name, const std::stop_token& /*stopToken*/) override
    {
        return record("start " + std::string(name));
    }
    [[nodiscard]] ServiceActionResult stop(std::string_view name, const std::stop_token& /*stopToken*/) override
    {
        return record("stop " + std::string(name));
    }
    [[nodiscard]] ServiceActionResult restart(std::string_view name, const std::stop_token& /*stopToken*/) override
    {
        return record("restart " + std::string(name));
    }
    [[nodiscard]] ServiceActionResult setStartType(std::string_view name, ServiceStartType /*startType*/) override
    {
        return record("setStartType " + std::string(name));
    }
    [[nodiscard]] ServiceActionResult runAuthorized(ServiceAction /*action*/,
                                                    std::string_view name,
                                                    ServiceStartType /*startType*/,
                                                    const std::stop_token& /*stopToken*/) override
    {
        return record("runAuthorized " + std::string(name));
    }

  private:
    ServiceActionResult record(std::string call)
    {
        calls.push_back(std::move(call));
        return answer;
    }
};

[[nodiscard]] ServiceActionResult needsAuthorization()
{
    ServiceActionResult refused = ServiceActionResult::failed("Requires administrator");
    refused.needsAuthorization = true;
    return refused;
}

struct Fixture
{
    explicit Fixture(bool connected) : broker(std::make_shared<Mocks::FakeBroker>(connected))
    {
        auto owned = std::make_unique<RecordingServiceActions>();
        inner = owned.get();
        actions = std::make_unique<BrokeredServiceActions>(std::move(owned), broker);
        broker->setAnswer([](const Broker::Request&)
                          { return Broker::Response{Broker::ServiceActionReply{.result = ServiceActionResult::succeeded()}}; });
    }
    std::shared_ptr<Mocks::FakeBroker> broker;
    RecordingServiceActions* inner = nullptr;
    std::unique_ptr<BrokeredServiceActions> actions;
};

TEST(BrokeredServiceActionsTest, ARefusalForWantOfAuthorizationGoesToARunningHelper)
{
    Fixture f(/*connected=*/true);
    f.inner->answer = needsAuthorization();
    const ServiceActionResult started = f.actions->start("cron.service", std::stop_token{});
    EXPECT_TRUE(started.ok);
    ASSERT_EQ(f.broker->requests().size(), 1U);
    EXPECT_EQ(f.broker->requests().front(),
              (Broker::Request{Broker::ServiceActionRequest{.action = ServiceAction::Start, .unit = "cron.service", .startType = {}}}));
    const ServiceActionResult typed = f.actions->setStartType("cron.service", ServiceStartType::Disabled);
    EXPECT_TRUE(typed.ok);
    EXPECT_EQ(f.broker->requests().back(),
              (Broker::Request{Broker::ServiceActionRequest{
                  .action = ServiceAction::SetStartType, .unit = "cron.service", .startType = ServiceStartType::Disabled}}));
    EXPECT_EQ(f.broker->connectCount(), 0) << "never a prompt";
}

// #3029: a helper's service action can take up to a minute; a stop request (cancelling, or closing
// TaskSmack) ends the wait for its answer, and the result says it was cancelled.
TEST(BrokeredServiceActionsTest, AStopRequestEndsTheWaitForTheHelper)
{
    Fixture f(/*connected=*/true);
    f.inner->answer = needsAuthorization();
    std::stop_source stop;
    stop.request_stop();
    const ServiceActionResult restarted = f.actions->restart("cron.service", stop.get_token());
    EXPECT_TRUE(restarted.cancelled);
    EXPECT_FALSE(restarted.ok);
    ASSERT_EQ(f.broker->requests().size(), 1U) << "the token reached the helper's call";

    const ServiceActionResult authorized =
        f.actions->runAuthorized(ServiceAction::Stop, "cron.service", ServiceStartType::Unknown, stop.get_token());
    EXPECT_TRUE(authorized.cancelled);
}

TEST(BrokeredServiceActionsTest, OtherFailuresAndNoRunningHelperLeaveTaskSmacksOwnAnswer)
{
    Fixture f(/*connected=*/true);
    f.inner->answer = ServiceActionResult::failed("Stopping it would also stop: ssh.service");
    EXPECT_EQ(f.actions->stop("cron.service", std::stop_token{}).message, "Stopping it would also stop: ssh.service");
    EXPECT_TRUE(f.broker->requests().empty()) << "not refused for want of authorization: not the helper's";

    f.broker->setConnected(false);
    f.inner->answer = needsAuthorization();
    const ServiceActionResult refused = f.actions->restart("cron.service", std::stop_token{});
    EXPECT_TRUE(refused.needsAuthorization) << "so the UI still offers Authenticate and retry";
    EXPECT_TRUE(f.broker->requests().empty());
    EXPECT_EQ(f.broker->connectCount(), 0);
}

TEST(BrokeredServiceActionsTest, AuthenticateAndRetryStartsTheHelperOnceAndActsThroughIt)
{
    Fixture f(/*connected=*/false);
    EXPECT_TRUE(f.actions->capabilities().canAuthorizeInteractively);
    const ServiceActionResult result =
        f.actions->runAuthorized(ServiceAction::Stop, "cron.service", ServiceStartType::Unknown, std::stop_token{});
    EXPECT_TRUE(result.ok) << result.message;
    EXPECT_EQ(f.broker->connectCount(), 1) << "one password";
    ASSERT_EQ(f.broker->requests().size(), 1U);
    EXPECT_TRUE(f.inner->calls.empty()) << "polkit never asked";
}

TEST(BrokeredServiceActionsTest, AHelperWithoutServiceActionsLeavesPolkitsPromptAndTheRefusal)
{
    Fixture f(/*connected=*/true);
    f.broker->setAnswer([](const Broker::Request&) { return Broker::Response{Broker::ErrorReply{.message = "not here"}}; });
    f.inner->answer = needsAuthorization();
    EXPECT_TRUE(f.actions->start("cron.service", std::stop_token{}).needsAuthorization) << "the refusal stands";
    static_cast<void>(f.actions->runAuthorized(ServiceAction::Start, "cron.service", ServiceStartType::Unknown, std::stop_token{}));
    ASSERT_FALSE(f.inner->calls.empty());
    EXPECT_EQ(f.inner->calls.back(), "runAuthorized cron.service") << "polkit's own prompt, as before";
}

TEST(BrokeredServiceActionsTest, ADismissedPasswordSaysSoAndNothingIsSent)
{
    Fixture f(/*connected=*/false);
    f.broker->setConnectFailure("dismissed");
    const ServiceActionResult result =
        f.actions->runAuthorized(ServiceAction::Start, "cron.service", ServiceStartType::Unknown, std::stop_token{});
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.authorizationUnavailable);
    EXPECT_TRUE(f.broker->requests().empty());
}

} // namespace
} // namespace Platform
