// Tests for BrokeredProcessActions (#2308): an action is tried in-process first, and only a permission
// refusal goes through the privileged helper -- at once while it runs, after runAuthorized()'s password
// prompt otherwise.

#include "Mocks/FakeBroker.h"
#include "Mocks/MockProbes.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokeredProcessActions.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"

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

TEST(BrokeredProcessActionsTest, WithoutABrokerTheActionsAreLeftAsTheyAre)
{
    auto owned = std::make_unique<TestMocks::MockProcessActions>();
    const IProcessActions* const raw = owned.get();
    const std::unique_ptr<IProcessActions> actions = withPrivilegedBroker(std::move(owned), nullptr);
    EXPECT_EQ(actions.get(), raw);
    EXPECT_FALSE(actions->actionCapabilities().canAuthorizeInteractively);
}

TEST(BrokeredProcessActionsTest, WithABrokerTheActionsCanAskForAuthorization)
{
    const std::unique_ptr<IProcessActions> actions =
        withPrivilegedBroker(std::make_unique<TestMocks::MockProcessActions>(), std::make_shared<Mocks::FakeBroker>(false));
    EXPECT_TRUE(actions->actionCapabilities().canAuthorizeInteractively);
}

// --- BrokeredEnvironmentReader (#2309) ---------------------------------------------------------------

struct EnvironmentFixture
{
    explicit EnvironmentFixture(bool connected) : broker(std::make_shared<Mocks::FakeBroker>(connected))
    {
        auto owned = std::make_unique<TestMocks::MockProcessEnvironmentReader>();
        inner = owned.get();
        reader = std::make_unique<BrokeredEnvironmentReader>(std::move(owned), broker);
        broker->setAnswer(
            [](const Broker::Request&)
            {
                return Broker::Response{Broker::EnvironmentReply{
                    .result = {.status = EnvironmentReadStatus::Ok, .variables = {{.name = "HOME", .value = "/root"}}}}};
            });
    }

    std::shared_ptr<Mocks::FakeBroker> broker;
    TestMocks::MockProcessEnvironmentReader* inner = nullptr;
    std::unique_ptr<BrokeredEnvironmentReader> reader;
};

TEST(BrokeredEnvironmentReaderTest, AnInProcessReadNeverReachesTheHelper)
{
    EnvironmentFixture f(/*connected=*/true);
    EXPECT_EQ(f.reader->readEnvironment(TARGET).status, EnvironmentReadStatus::Ok);
    EXPECT_TRUE(f.broker->requests().empty());
}

TEST(BrokeredEnvironmentReaderTest, ARefusedReadGoesThroughARunningHelper)
{
    EnvironmentFixture f(/*connected=*/true);
    f.inner->setResult({.status = EnvironmentReadStatus::PermissionDenied, .variables = {}});
    const EnvironmentReadResult result = f.reader->readEnvironment(TARGET);
    EXPECT_EQ(result.status, EnvironmentReadStatus::Ok);
    ASSERT_EQ(result.variables.size(), 1U);
    ASSERT_EQ(f.broker->requests().size(), 1U);
    EXPECT_EQ(f.broker->requests().front(), (Broker::Request{Broker::EnvironmentRequest{.target = TARGET}}));
}

TEST(BrokeredEnvironmentReaderTest, ARefusedReadWithoutAHelperIsReturnedForTheUiToOffer)
{
    EnvironmentFixture f(/*connected=*/false);
    f.inner->setResult({.status = EnvironmentReadStatus::PermissionDenied, .variables = {}});
    EXPECT_EQ(f.reader->readEnvironment(TARGET).status, EnvironmentReadStatus::PermissionDenied);
    EXPECT_TRUE(f.reader->canReadAuthorized());
    EXPECT_EQ(f.broker->connectCount(), 0);
}

TEST(BrokeredEnvironmentReaderTest, ReadAuthorizedConnectsThenReadsThroughTheHelper)
{
    EnvironmentFixture f(/*connected=*/false);
    EXPECT_EQ(f.reader->readAuthorized(TARGET, std::stop_token{}).status, EnvironmentReadStatus::Ok);
    EXPECT_EQ(f.broker->connectCount(), 1);
}

TEST(BrokeredEnvironmentReaderTest, ADismissedPromptLeavesTheRefusal)
{
    EnvironmentFixture f(/*connected=*/false);
    f.broker->setConnectFailure("the password prompt was dismissed");
    EXPECT_EQ(f.reader->readAuthorized(TARGET, std::stop_token{}).status, EnvironmentReadStatus::PermissionDenied);
    EXPECT_TRUE(f.broker->requests().empty());
}

// --- BrokeredModulesReader and BrokeredOpenFilesReader (#2309) ----------------------------------------

TEST(BrokeredModulesReaderTest, ARefusedReadGoesThroughARunningHelperAndOtherwiseWaitsForTheUi)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(true);
    broker->setAnswer(
        [](const Broker::Request&)
        { return Broker::Response{Broker::ModulesReply{.result = {.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}}}}; });
    auto owned = std::make_unique<TestMocks::MockProcessModulesReader>();
    owned->setResult({.status = ModulesReadStatus::PermissionDenied, .modules = {}, .detail = {}});
    BrokeredModulesReader reader(std::move(owned), broker);
    EXPECT_EQ(reader.readModules(TARGET).status, ModulesReadStatus::Ok);
    ASSERT_EQ(broker->requests().size(), 1U);
    EXPECT_EQ(broker->requests().front(), (Broker::Request{Broker::ModulesRequest{.target = TARGET}}));

    broker->setConnected(false);
    EXPECT_EQ(reader.readModules(TARGET).status, ModulesReadStatus::PermissionDenied);
    EXPECT_TRUE(reader.canReadAuthorized());
    EXPECT_EQ(reader.readAuthorized(TARGET, std::stop_token{}).status, ModulesReadStatus::Ok);
    EXPECT_EQ(broker->connectCount(), 1);
}

TEST(BrokeredOpenFilesReaderTest, ARefusedReadGoesThroughARunningHelperAndADismissedPromptLeavesTheRefusal)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(true);
    broker->setAnswer(
        [](const Broker::Request&)
        {
            OpenFilesReadResult ok;
            ok.status = OpenFilesReadStatus::Ok;
            return Broker::Response{Broker::OpenFilesReply{.result = ok}};
        });
    auto owned = std::make_unique<TestMocks::MockProcessOpenFilesReader>();
    OpenFilesReadResult refused;
    refused.status = OpenFilesReadStatus::PermissionDenied;
    owned->setResult(refused);
    BrokeredOpenFilesReader reader(std::move(owned), broker);
    EXPECT_EQ(reader.readOpenFiles(TARGET).status, OpenFilesReadStatus::Ok);

    broker->setConnected(false);
    broker->setConnectFailure("the password prompt was dismissed");
    EXPECT_EQ(reader.readAuthorized(TARGET, std::stop_token{}).status, OpenFilesReadStatus::PermissionDenied);
    EXPECT_EQ(broker->requests().size(), 1U); // only the first
}

} // namespace
} // namespace Platform
