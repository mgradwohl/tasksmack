// Tests for BrokeredProcessActions (#2308): an action is tried in-process first, and only a permission
// refusal goes through the privileged helper -- at once while it runs, after runAuthorized()'s password
// prompt otherwise.

#include "Mocks/FakeBroker.h"
#include "Mocks/MockProbes.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokeredProcessActions.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace Platform
{
namespace
{

constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 99};

/// The decorator over a mock whose actions the test scripts, and a fake broker.
struct Fixture
{
    explicit Fixture(bool connected) : broker(std::make_shared<Mocks::FakeBroker>(connected))
    {
        auto owned = std::make_unique<TestMocks::MockProcessActions>();
        owned->setCapabilities({.canTerminate = true, .canKill = true, .canSetPriority = true, .canSetIoPriority = true});
        inner = owned.get();
        actions = std::make_unique<BrokeredProcessActions>(std::move(owned), broker);
    }

    std::shared_ptr<Mocks::FakeBroker> broker;
    TestMocks::MockProcessActions* inner = nullptr;
    std::unique_ptr<BrokeredProcessActions> actions;
};

TEST(BrokeredProcessActionsTest, AnInProcessSuccessNeverReachesTheHelper)
{
    Fixture f(/*connected=*/true);
    EXPECT_TRUE(f.actions->kill(TARGET).success);
    EXPECT_EQ(f.inner->killCount(), 1);
    EXPECT_TRUE(f.broker->requests().empty());
}

TEST(BrokeredProcessActionsTest, AFailureOtherThanPermissionIsNotRetried)
{
    Fixture f(/*connected=*/true);
    f.inner->setKillResult(ProcessActionResult::error("Process not found - may have already exited"));
    const ProcessActionResult result = f.actions->kill(TARGET);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Process not found - may have already exited");
    EXPECT_TRUE(f.broker->requests().empty());
}

TEST(BrokeredProcessActionsTest, APermissionRefusalIsRetriedThroughARunningHelper)
{
    Fixture f(/*connected=*/true);
    f.inner->setKillResult(ProcessActionResult::denied("Permission denied - process belongs to another user"));
    EXPECT_TRUE(f.actions->kill(TARGET).success);
    const std::vector<Broker::Request> requests = f.broker->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front(), (Broker::Request{Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill}}));
    EXPECT_EQ(f.broker->connectCount(), 0); // no prompt while it runs
}

TEST(BrokeredProcessActionsTest, APermissionRefusalWithoutAHelperIsReturnedForTheUiToOffer)
{
    Fixture f(/*connected=*/false);
    f.inner->setPriorityResult(ProcessActionResult::denied("Permission denied"));
    const ProcessActionResult result = f.actions->setPriority(TARGET, -5);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.permissionDenied);
    EXPECT_TRUE(f.actions->actionCapabilities().canAuthorizeInteractively);
    EXPECT_TRUE(f.broker->requests().empty());
    EXPECT_EQ(f.broker->connectCount(), 0); // never prompts by itself
}

TEST(BrokeredProcessActionsTest, RunAuthorizedConnectsThenActsThroughTheHelper)
{
    Fixture f(/*connected=*/false);
    const ProcessActionResult result = f.actions->runAuthorized(
        {.kind = AuthorizedAction::Kind::SetIoPriority, .target = TARGET, .ioClass = IoPriorityClass::Realtime, .level = 2},
        std::stop_token{});
    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(f.broker->connectCount(), 1);
    const std::vector<Broker::Request> requests = f.broker->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front(),
              (Broker::Request{Broker::IoPriorityRequest{.target = TARGET, .ioClass = IoPriorityClass::Realtime, .level = 2}}));
    EXPECT_EQ(f.inner->setIoPriorityCount(), 0); // straight to the helper: the in-process attempt already failed
}

TEST(BrokeredProcessActionsTest, RunAuthorizedReportsADismissedPrompt)
{
    Fixture f(/*connected=*/false);
    f.broker->setConnectFailure("the password prompt was dismissed");
    const ProcessActionResult result =
        f.actions->runAuthorized({.kind = AuthorizedAction::Kind::Kill, .target = TARGET}, std::stop_token{});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Couldn't act as an administrator: the password prompt was dismissed");
    EXPECT_FALSE(result.permissionDenied); // nothing to offer again straight away
    EXPECT_TRUE(f.broker->requests().empty());
}

TEST(BrokeredProcessActionsTest, TheHelpersRefusalIsNotOfferedAgain)
{
    Fixture f(/*connected=*/true);
    f.inner->setKillResult(ProcessActionResult::denied("Permission denied"));
    f.broker->setAnswer(
        [](const Broker::Request&)
        { return Broker::Response{Broker::ActionResult{.success = false, .errorMessage = "Operation not permitted", .successNote = {}}}; });
    const ProcessActionResult result = f.actions->kill(TARGET);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.permissionDenied); // an administrator was refused too: not curable
}

TEST(BrokeredProcessActionsTest, EachActionMapsToItsRequest)
{
    const CpuAffinity cpus = CpuAffinity::fromMask(0b11);
    EXPECT_EQ((brokerRequestFor({.kind = AuthorizedAction::Kind::Terminate, .target = TARGET})),
              (Broker::Request{Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Terminate}}));
    EXPECT_EQ((brokerRequestFor({.kind = AuthorizedAction::Kind::Stop, .target = TARGET})),
              (Broker::Request{Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Suspend}}));
    EXPECT_EQ((brokerRequestFor({.kind = AuthorizedAction::Kind::Resume, .target = TARGET})),
              (Broker::Request{Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Resume}}));
    EXPECT_EQ((brokerRequestFor({.kind = AuthorizedAction::Kind::SetPriority, .target = TARGET, .nice = -10})),
              (Broker::Request{Broker::PriorityRequest{.target = TARGET, .nice = -10}}));
    EXPECT_EQ((brokerRequestFor({.kind = AuthorizedAction::Kind::SetAffinity, .target = TARGET, .cpus = cpus})),
              (Broker::Request{Broker::AffinityRequest{.target = TARGET, .cpus = cpus}}));
}

TEST(BrokeredProcessActionsTest, TheRealtimeIoClassIsOfferedWhileTheHelperRuns)
{
    Fixture f(/*connected=*/false);
    EXPECT_FALSE(f.actions->actionCapabilities().canSetRealtimeIoPriority);
    f.broker->setConnected(true);
    EXPECT_TRUE(f.actions->actionCapabilities().canSetRealtimeIoPriority);
}

TEST(BrokeredProcessActionsTest, ReadsAndOtherCallsAreTheInProcessOnes)
{
    Fixture f(/*connected=*/true);
    [[maybe_unused]] const IoPriorityReadResult read = f.actions->getIoPriority(TARGET);
    EXPECT_EQ(f.inner->getIoPriorityCount(), 1);
    EXPECT_TRUE(f.broker->requests().empty());
}

} // namespace
} // namespace Platform
