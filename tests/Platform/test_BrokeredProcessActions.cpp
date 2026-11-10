// Tests for BrokeredProcessActions (#2308): an action is tried in-process first, and only a permission
// refusal goes through the privileged helper -- at once while it runs, after runAuthorized()'s password
// prompt otherwise.

#include "Mocks/FakeBroker.h"
#include "Mocks/MockProbes.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokeredProcessActions.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessThreads.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
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
    const Fixture f(/*connected=*/true);
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

// #3429: Windows' quiet High for an unelevated Realtime is a permission refusal, so a running helper, elevated,
// sets Realtime's value as any priority change; without one the UI offers Retry as administrator.
TEST(BrokeredProcessActionsTest, ARealtimeThatCameOutHighGoesThroughTheHelper)
{
    const Fixture f(/*connected=*/true);
    f.inner->setPriorityResult(ProcessActionResult::denied("Windows set High: Realtime needs administrator rights."));
    const ProcessActionResult result = f.actions->setPriority(TARGET, -20);
    EXPECT_TRUE(result.success) << result.errorMessage;
    const std::vector<Broker::Request> requests = f.broker->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front(), (Broker::Request{Broker::PriorityRequest{.target = TARGET, .nice = -20}}));

    const Fixture offline(/*connected=*/false);
    offline.inner->setPriorityResult(ProcessActionResult::denied("Windows set High: Realtime needs administrator rights."));
    const ProcessActionResult refused = offline.actions->setPriority(TARGET, -20);
    EXPECT_TRUE(refused.permissionDenied);
    EXPECT_EQ(refused.errorMessage, "Windows set High: Realtime needs administrator rights.");
    EXPECT_TRUE(offline.broker->requests().empty());
}

TEST(BrokeredProcessActionsTest, RunAuthorizedConnectsThenActsThroughTheHelper)
{
    const Fixture f(/*connected=*/false);
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
    const Fixture f(/*connected=*/false);
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

TEST(BrokeredProcessActionsTest, AHelperThatCantBeAskedLeavesTheRefusalToOfferAgain)
{
    // connected() said yes, but the helper idle-exited or timed out before answering (#2414).
    Fixture f(/*connected=*/true);
    f.inner->setTerminateResult(ProcessActionResult::denied("Permission denied"));
    f.broker->setAnswer([](const Broker::Request&) -> std::expected<Broker::Response, std::string>
                        { return std::unexpected(std::string("the helper exited")); });
    const ProcessActionResult result = f.actions->terminate(TARGET);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.permissionDenied); // a fresh helper could still do it: Authenticate and retry
    EXPECT_EQ(result.errorMessage, "Permission denied (the administrator helper couldn't be asked: the helper exited)");
    EXPECT_EQ(f.broker->requests().size(), 1U);
    EXPECT_EQ(f.broker->connectCount(), 0); // never prompts by itself
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
    // #3580: protocol v23 has no SetSchedPolicy, Linux's, whose helper is gone: no request does it.
    EXPECT_FALSE(
        brokerRequestFor({.kind = AuthorizedAction::Kind::SetSchedPolicy, .target = TARGET, .schedPolicy = SchedPolicy::Idle}).has_value());
}

// #3580: the Realtime I/O class and real-time scheduling are the in-process actions' own: a running helper adds
// neither, as Linux's did.
TEST(BrokeredProcessActionsTest, ARunningHelperAddsNoRealtimeCapability)
{
    const Fixture f(/*connected=*/true);
    f.inner->setCapabilities({.canSetIoPriority = true, .canSetSchedPolicy = true});
    EXPECT_FALSE(f.actions->actionCapabilities().canSetRealtimeIoPriority);
    EXPECT_FALSE(f.actions->actionCapabilities().canSetRealtimeSchedPolicy);
    f.inner->setCapabilities(
        {.canSetIoPriority = true, .canSetRealtimeIoPriority = true, .canSetSchedPolicy = true, .canSetRealtimeSchedPolicy = true});
    EXPECT_TRUE(f.actions->actionCapabilities().canSetRealtimeIoPriority);
    EXPECT_TRUE(f.actions->actionCapabilities().canSetRealtimeSchedPolicy);
}

// #3182: the capabilities are asked for every frame on the UI thread, so they never call connected(), which peeks
// at the pipe.
TEST(BrokeredProcessActionsTest, TheCapabilitiesNeverLookAtTheHelpersChannel)
{
    const Fixture f(/*connected=*/true);
    for (int frame = 0; frame < 100; ++frame)
    {
        EXPECT_TRUE(f.actions->actionCapabilities().canAuthorizeInteractively);
    }
    EXPECT_EQ(f.broker->connectedCallCount(), 0);

    // An action still looks, on the worker it runs on, before it retries through the helper: here it finds it gone.
    f.broker->setConnected(false);
    f.inner->setKillResult(ProcessActionResult::denied("Permission denied"));
    EXPECT_FALSE(f.actions->kill(TARGET).success);
    EXPECT_EQ(f.broker->connectedCallCount(), 1);
}

// Where the platform can't set an I/O priority at all (Windows), the connection isn't even read.
TEST(BrokeredProcessActionsTest, WithoutIoPriorityTheRealtimeClassIsNeverOffered)
{
    auto owned = std::make_unique<TestMocks::MockProcessActions>();
    owned->setCapabilities({.canTerminate = true, .canKill = true, .canSetPriority = true});
    const auto broker = std::make_shared<Mocks::FakeBroker>(/*connected=*/true);
    const BrokeredProcessActions actions(std::move(owned), broker);
    EXPECT_FALSE(actions.actionCapabilities().canSetRealtimeIoPriority);
    EXPECT_TRUE(actions.actionCapabilities().canAuthorizeInteractively);
    EXPECT_EQ(broker->connectedCallCount(), 0);
}

TEST(BrokeredProcessActionsTest, ReadsAndOtherCallsAreTheInProcessOnes)
{
    const Fixture f(/*connected=*/true);
    [[maybe_unused]] const IoPriorityReadResult read = f.actions->getIoPriority(TARGET);
    EXPECT_EQ(f.inner->getIoPriorityCount(), 1);
    EXPECT_TRUE(f.broker->requests().empty());
}

TEST(BrokeredProcessActionsTest, EachSignalMapsToTheHelpersSignal)
{
    EXPECT_EQ(brokerSignalFor(ProcessSignal::Hangup), Broker::Signal::Hangup);
    EXPECT_EQ(brokerSignalFor(ProcessSignal::Interrupt), Broker::Signal::Interrupt);
    EXPECT_EQ(brokerSignalFor(ProcessSignal::Quit), Broker::Signal::Quit);
    EXPECT_EQ(brokerSignalFor(ProcessSignal::User1), Broker::Signal::User1);
    EXPECT_EQ(brokerSignalFor(ProcessSignal::User2), Broker::Signal::User2);
}

TEST(BrokeredProcessActionsTest, OpeningTheFileLocationAndRunningAProgramStayInProcessEvenWhenRefused)
{
    // Both run as the user (the file manager; run's own elevation prompt), so a refusal is never retried
    // through the helper, even while it runs.
    const Fixture f(/*connected=*/true);
    f.inner->setOpenFileLocationResult(ProcessActionResult::denied("no access"));
    f.inner->setRunProgramResult(ProcessActionResult::denied("no access"));

    const ProcessActionResult opened = f.actions->openFileLocation(TARGET);
    EXPECT_FALSE(opened.success);
    EXPECT_EQ(f.inner->openFileLocationCount(), 1);

    const ProcessActionResult ran = f.actions->runProgram({.commandLine = "true --flag", .asAdministrator = true});
    EXPECT_FALSE(ran.success);
    EXPECT_EQ(f.inner->runProgramCount(), 1);
    EXPECT_EQ(f.inner->lastRunProgram().commandLine, "true --flag");
    EXPECT_TRUE(f.inner->lastRunProgram().asAdministrator);

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
    const EnvironmentFixture f(/*connected=*/true);
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
    const EnvironmentFixture f(/*connected=*/false);
    EXPECT_EQ(f.reader->readAuthorized(TARGET, std::stop_token{}).status, EnvironmentReadStatus::Ok);
    EXPECT_EQ(f.broker->connectCount(), 1);
}

TEST(BrokeredEnvironmentReaderTest, ADismissedPromptLeavesTheRefusal)
{
    const EnvironmentFixture f(/*connected=*/false);
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

// #2550 (#2577 review, finding 1): where the helper would run as another account and read only the
// user's own processes (a standard user's Windows TaskSmack), no reader offers a prompt that can't help,
// and none prompts; refused actions are still offered the retry.
TEST(BrokeredModulesReaderTest, AHelperThatReadsOnlyTheUsersOwnProcessesIsNotOfferedForReads)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(false);
    broker->setReadsOtherUsers(false);

    auto modules = std::make_unique<TestMocks::MockProcessModulesReader>();
    modules->setResult({.status = ModulesReadStatus::PermissionDenied, .modules = {}, .detail = {}});
    BrokeredModulesReader modulesReader(std::move(modules), broker);
    EXPECT_FALSE(modulesReader.canReadAuthorized());
    EXPECT_EQ(modulesReader.readAuthorized(TARGET, std::stop_token{}).status, ModulesReadStatus::PermissionDenied);

    auto openFiles = std::make_unique<TestMocks::MockProcessOpenFilesReader>();
    const BrokeredOpenFilesReader openFilesReader(std::move(openFiles), broker);
    EXPECT_FALSE(openFilesReader.canReadAuthorized());

    auto connections = std::make_unique<TestMocks::MockProcessConnectionsReader>();
    const BrokeredConnectionsReader connectionsReader(std::move(connections), broker);
    EXPECT_FALSE(connectionsReader.canReadAuthorized());

    auto environment = std::make_unique<TestMocks::MockProcessEnvironmentReader>();
    const BrokeredEnvironmentReader environmentReader(std::move(environment), broker);
    EXPECT_FALSE(environmentReader.canReadAuthorized());
    EXPECT_EQ(broker->connectCount(), 0) << "no prompt for a read that can't be helped";

    const Fixture f(/*connected=*/false);
    f.broker->setReadsOtherUsers(false);
    EXPECT_TRUE(f.actions->actionCapabilities().canAuthorizeInteractively) << "an action is still offered the retry";
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

TEST(BrokeredConnectionsReaderTest, ARefusedReadGoesThroughARunningHelperAndOtherwiseWaitsForTheUi)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(true);
    broker->setAnswer(
        [](const Broker::Request&)
        {
            return Broker::Response{
                Broker::ConnectionsReply{.result = {.status = ConnectionsReadStatus::Ok, .connections = {}, .detail = {}}}};
        });
    auto owned = std::make_unique<TestMocks::MockProcessConnectionsReader>();
    owned->setResult({.status = ConnectionsReadStatus::PermissionDenied, .connections = {}, .detail = {}});
    BrokeredConnectionsReader reader(std::move(owned), broker);
    EXPECT_EQ(reader.readConnections(TARGET).status, ConnectionsReadStatus::Ok);
    ASSERT_EQ(broker->requests().size(), 1U);
    EXPECT_EQ(broker->requests().front(), (Broker::Request{Broker::ConnectionsRequest{.target = TARGET}}));

    broker->setConnected(false);
    EXPECT_EQ(reader.readConnections(TARGET).status, ConnectionsReadStatus::PermissionDenied);
    EXPECT_TRUE(reader.canReadAuthorized());
    EXPECT_EQ(reader.readAuthorized(TARGET, std::stop_token{}).status, ConnectionsReadStatus::Ok);
    EXPECT_EQ(broker->connectCount(), 1);
}

// --- Every action, and every call the decorators only pass on (#2417) --------------------------------

TEST(BrokeredProcessActionsTest, EverySignalRefusedInProcessIsRetriedAsItsOwnRequest)
{
    Fixture f(/*connected=*/true);
    const ProcessActionResult denied = ProcessActionResult::denied("Permission denied");
    f.inner->setTerminateResult(denied);
    f.inner->setStopResult(denied);
    f.inner->setResumeResult(denied);
    f.inner->setIoPriorityResult(denied);

    EXPECT_TRUE(f.actions->terminate(TARGET).success);
    EXPECT_TRUE(f.actions->stop(TARGET).success);
    EXPECT_TRUE(f.actions->resume(TARGET).success);
    EXPECT_TRUE(f.actions->setIoPriority(TARGET, IoPriorityClass::Idle, 7).success);
    EXPECT_EQ(f.inner->terminateCount(), 1);
    EXPECT_EQ(f.inner->stopCount(), 1);
    EXPECT_EQ(f.inner->resumeCount(), 1);
    EXPECT_EQ(f.inner->setIoPriorityCount(), 1);

    const std::vector<Broker::Request> requests = f.broker->requests();
    ASSERT_EQ(requests.size(), 4U);
    EXPECT_EQ(requests[0], (Broker::Request{Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Terminate}}));
    EXPECT_EQ(requests[1], (Broker::Request{Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Suspend}}));
    EXPECT_EQ(requests[2], (Broker::Request{Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Resume}}));
    EXPECT_EQ(requests[3], (Broker::Request{Broker::IoPriorityRequest{.target = TARGET, .ioClass = IoPriorityClass::Idle, .level = 7}}));
}

// #3063: a signal TaskSmack may not send (another user's process) is retried through the helper as a
// Signal request carrying that signal; one it may send never reaches the helper.
TEST(BrokeredProcessActionsTest, AnOtherSignalRefusedInProcessIsRetriedThroughTheHelper)
{
    Fixture f(/*connected=*/true);
    EXPECT_TRUE(f.actions->sendSignal(TARGET, ProcessSignal::Interrupt).success);
    EXPECT_TRUE(f.broker->requests().empty()) << "sent in process: the helper isn't asked";

    f.inner->setSendSignalResult(ProcessActionResult::denied("Permission denied"));
    EXPECT_TRUE(f.actions->sendSignal(TARGET, ProcessSignal::User1).success);
    EXPECT_EQ(f.inner->sendSignalCount(), 2);
    const std::vector<Broker::Request> requests = f.broker->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0], (Broker::Request{Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::User1}}));

    EXPECT_EQ((brokerRequestFor({.kind = AuthorizedAction::Kind::SendSignal, .target = TARGET, .signal = ProcessSignal::Hangup})),
              (Broker::Request{Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Hangup}}));
}

// #3071: Efficiency mode on a process TaskSmack may not change (elevated, another user's) is retried through
// the helper as a SetEfficiencyMode request carrying on or off; one changed in process never reaches it.
TEST(BrokeredProcessActionsTest, EfficiencyModeRefusedInProcessIsRetriedThroughTheHelper)
{
    Fixture f(/*connected=*/true);
    EXPECT_TRUE(f.actions->setEfficiencyMode(TARGET, true).success);
    EXPECT_TRUE(f.inner->lastEfficiencyMode());
    EXPECT_TRUE(f.broker->requests().empty()) << "changed in process: the helper isn't asked";

    f.inner->setEfficiencyModeResult(ProcessActionResult::denied("Access denied"));
    EXPECT_TRUE(f.actions->setEfficiencyMode(TARGET, false).success);
    EXPECT_EQ(f.inner->efficiencyModeCount(), 2);
    EXPECT_FALSE(f.inner->lastEfficiencyMode());
    const std::vector<Broker::Request> requests = f.broker->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0], (Broker::Request{Broker::EfficiencyModeRequest{.target = TARGET, .on = false}}));

    EXPECT_EQ((brokerRequestFor({.kind = AuthorizedAction::Kind::SetEfficiencyMode, .target = TARGET, .efficiencyMode = true})),
              (Broker::Request{Broker::EfficiencyModeRequest{.target = TARGET, .on = true}}));
}

// #3296: an off refused in process, whose on ran in process, is finished as a priority change to the nice that
// on found, not as an off the helper would end at nice 0; then the in-process off runs again for the weight and
// the memory. Without a nice to put back it stays a plain off.
TEST(BrokeredProcessActionsTest, AnEfficiencyOffThroughTheHelperPutsBackTheRememberedNice)
{
    Fixture f(/*connected=*/true);
    f.inner->setEfficiencyNiceToRestore(10);
    f.inner->setEfficiencyModeResult(ProcessActionResult::denied("Permission denied"));
    const ProcessActionResult retried = f.actions->setEfficiencyMode(TARGET, false);
    EXPECT_TRUE(retried.success) << retried.errorMessage;
    ASSERT_EQ(f.broker->requests().size(), 1U);
    EXPECT_EQ(f.broker->requests()[0], (Broker::Request{Broker::PriorityRequest{.target = TARGET, .nice = 10}}));
    EXPECT_EQ(f.inner->efficiencyModeCount(), 2) << "the refused off, then the one that finishes it";
    EXPECT_TRUE(retried.successNote.contains("CPU weight wasn't put back: Permission denied")) << retried.successNote;

    // "Authenticate and retry" sends the same, and the finishing off's note is kept.
    f.inner->setEfficiencyModeResult(ProcessActionResult::ok("its cgroup's CPU weight is still the minimum"));
    const ProcessActionResult authorized = f.actions->runAuthorized(
        {.kind = AuthorizedAction::Kind::SetEfficiencyMode, .target = TARGET, .efficiencyMode = false}, std::stop_token{});
    EXPECT_TRUE(authorized.success) << authorized.errorMessage;
    ASSERT_EQ(f.broker->requests().size(), 2U);
    EXPECT_EQ(f.broker->requests()[1], (Broker::Request{Broker::PriorityRequest{.target = TARGET, .nice = 10}}));
    EXPECT_EQ(f.inner->efficiencyModeCount(), 3);
    EXPECT_EQ(authorized.successNote, "its cgroup's CPU weight is still the minimum");

    // An on goes as SetEfficiencyMode and isn't followed in process.
    ASSERT_TRUE(f.actions
                    ->runAuthorized({.kind = AuthorizedAction::Kind::SetEfficiencyMode, .target = TARGET, .efficiencyMode = true},
                                    std::stop_token{})
                    .success);
    EXPECT_EQ(f.broker->requests()[2], (Broker::Request{Broker::EfficiencyModeRequest{.target = TARGET, .on = true}}));
    EXPECT_EQ(f.inner->efficiencyModeCount(), 3);

    // No nice left to put back (changed since the refusal): the in-process off is tried first, and when it
    // succeeds it is the result, the helper unasked; only one still refused goes to the helper as a plain off.
    f.inner->setEfficiencyNiceToRestore(std::nullopt);
    const AuthorizedAction off{.kind = AuthorizedAction::Kind::SetEfficiencyMode, .target = TARGET, .efficiencyMode = false};
    EXPECT_EQ(f.actions->runAuthorized(off, std::stop_token{}).successNote, "its cgroup's CPU weight is still the minimum");
    EXPECT_EQ(f.broker->requests().size(), 3U);
    EXPECT_EQ(f.inner->efficiencyModeCount(), 4);
    f.inner->setEfficiencyModeResult(ProcessActionResult::denied("Permission denied"));
    ASSERT_TRUE(f.actions->runAuthorized(off, std::stop_token{}).success);
    ASSERT_EQ(f.broker->requests().size(), 4U);
    EXPECT_EQ(f.broker->requests()[3], (Broker::Request{Broker::EfficiencyModeRequest{.target = TARGET, .on = false}}));
    EXPECT_EQ(f.inner->efficiencyModeCount(), 5);
}

// #3327 (Windows): the helper's on replies with the class it replaced, and TaskSmack remembers it; the off the
// helper finishes carries that class back, so the helper restores it though it never saw the on (or exited since),
// and TaskSmack forgets it once that off succeeds. The retry and "Authenticate and retry" alike.
TEST(BrokeredProcessActionsTest, TheHelpersOnIsRememberedInProcessAndItsOffCarriesTheClassBack)
{
    constexpr std::uint32_t HIGH_CLASS = 0x80;
    const Fixture f(/*connected=*/true);
    f.broker->setAnswer(
        [](const Broker::Request& request) -> std::expected<Broker::Response, std::string>
        {
            const auto& efficiency = std::get<Broker::EfficiencyModeRequest>(request);
            return Broker::ActionResult{
                .success = true,
                .errorMessage = {},
                .successNote = {},
                .priorityClassBefore = efficiency.on ? HIGH_CLASS : 0U,
            };
        });
    f.inner->setEfficiencyModeResult(ProcessActionResult::denied("Access denied"));

    ASSERT_TRUE(f.actions->setEfficiencyMode(TARGET, true).success);
    EXPECT_EQ(f.inner->efficiencyPriorityClassToRestore(TARGET), std::optional<std::uint32_t>{HIGH_CLASS}) << "remembered here";

    ASSERT_TRUE(f.actions->setEfficiencyMode(TARGET, false).success);
    ASSERT_EQ(f.broker->requests().size(), 2U);
    EXPECT_EQ(f.broker->requests()[1],
              (Broker::Request{Broker::EfficiencyModeRequest{.target = TARGET, .on = false, .restorePriorityClass = HIGH_CLASS}}));
    EXPECT_FALSE(f.inner->efficiencyPriorityClassToRestore(TARGET).has_value()) << "spent";
    EXPECT_EQ(f.inner->efficiencyLog(), (std::vector<std::string>{"on", "remember 0x80", "off", "forget"}));

    // "Authenticate and retry": the in-process off is tried first, then the helper's carries the class.
    f.inner->rememberEfficiencyPriorityClass(TARGET, HIGH_CLASS);
    const AuthorizedAction off{.kind = AuthorizedAction::Kind::SetEfficiencyMode, .target = TARGET, .efficiencyMode = false};
    ASSERT_TRUE(f.actions->runAuthorized(off, std::stop_token{}).success);
    EXPECT_EQ(f.broker->requests().back(),
              (Broker::Request{Broker::EfficiencyModeRequest{.target = TARGET, .on = false, .restorePriorityClass = HIGH_CLASS}}));
    EXPECT_FALSE(f.inner->efficiencyPriorityClassToRestore(TARGET).has_value());

    // Another process given the same PID (a reused one) has nothing remembered: its off carries 0, and the helper
    // falls back to what it remembers itself, or Normal, saying so.
    f.inner->rememberEfficiencyPriorityClass(TARGET, HIGH_CLASS);
    const ProcessTarget reused{.pid = TARGET.pid, .startTimeTicks = TARGET.startTimeTicks + 1};
    ASSERT_TRUE(f.actions->setEfficiencyMode(reused, false).success);
    EXPECT_EQ(f.broker->requests().back(), (Broker::Request{Broker::EfficiencyModeRequest{.target = reused, .on = false}}));
    EXPECT_EQ(f.inner->efficiencyPriorityClassToRestore(TARGET), std::optional<std::uint32_t>{HIGH_CLASS}) << "the first's is kept";
}

// #3327: a helper's on or off that fails remembers and forgets nothing, and an on whose class the helper couldn't
// read (0) isn't remembered.
TEST(BrokeredProcessActionsTest, AFailedOrUnreadHelperEfficiencyChangeLeavesTheMemoryAlone)
{
    const Fixture f(/*connected=*/true);
    f.inner->setEfficiencyModeResult(ProcessActionResult::denied("Access denied"));
    f.broker->setAnswer([](const Broker::Request&) -> std::expected<Broker::Response, std::string>
                        { return Broker::ActionResult{.success = false, .errorMessage = "Process not found", .successNote = {}}; });
    f.inner->rememberEfficiencyPriorityClass(TARGET, 0x4000);
    EXPECT_FALSE(f.actions->setEfficiencyMode(TARGET, false).success);
    EXPECT_EQ(f.inner->efficiencyPriorityClassToRestore(TARGET), std::optional<std::uint32_t>{0x4000}) << "a failed off keeps it";

    f.broker->setAnswer([](const Broker::Request&) -> std::expected<Broker::Response, std::string>
                        { return Broker::ActionResult{.success = true, .errorMessage = {}, .successNote = {}}; });
    const ProcessTarget other{.pid = 77, .startTimeTicks = 5};
    ASSERT_TRUE(f.actions->setEfficiencyMode(other, true).success);
    EXPECT_FALSE(f.inner->efficiencyPriorityClassToRestore(other).has_value()) << "no class in the reply";
    EXPECT_EQ(f.inner->efficiencyLog(), (std::vector<std::string>{"remember 0x4000", "off", "on"}));
}

// #3066: closing another user's process's socket, refused in process, is retried through the helper as a
// CloseConnection request carrying the inode; one closed in process never reaches it.
TEST(BrokeredProcessActionsTest, CloseConnectionRefusedInProcessIsRetriedThroughTheHelper)
{
    Fixture f(/*connected=*/true);
    EXPECT_TRUE(f.actions->closeConnection(TARGET, 555).success);
    EXPECT_TRUE(f.broker->requests().empty()) << "closed in process: the helper isn't asked";

    f.inner->setCloseConnectionResult(ProcessActionResult::denied("Operation not permitted"));
    EXPECT_TRUE(f.actions->closeConnection(TARGET, 556).success);
    EXPECT_EQ(f.inner->closeConnectionCount(), 2);
    const std::vector<Broker::Request> requests = f.broker->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0], (Broker::Request{Broker::CloseConnectionRequest{.target = TARGET, .socketInode = 556}}));
    EXPECT_EQ((brokerRequestFor({.kind = AuthorizedAction::Kind::CloseConnection, .target = TARGET, .socketInode = 9})),
              (Broker::Request{Broker::CloseConnectionRequest{.target = TARGET, .socketInode = 9}}));
}

// #3358: on Windows the connection is named by its IPv4 TCP endpoints. Refused in process (SetTcpEntry needs an
// administrator), it is retried through the helper as a CloseConnection request carrying them, with no inode.
TEST(BrokeredProcessActionsTest, ATcpConnectionRefusedInProcessIsRetriedThroughTheHelperByItsEndpoints)
{
    const TcpConnectionToClose connection{
        .localAddress = {10, 0, 0, 5},
        .localPort = 52144,
        .remoteAddress = {142, 250, 80, 46},
        .remotePort = 443,
    };
    const Fixture f(/*connected=*/true);
    EXPECT_TRUE(f.actions->closeTcpConnection(TARGET, connection).success);
    EXPECT_TRUE(f.broker->requests().empty()) << "closed in process: the helper isn't asked";

    f.inner->setCloseConnectionResult(ProcessActionResult::denied("Closing a connection needs administrator rights"));
    EXPECT_TRUE(f.actions->closeTcpConnection(TARGET, connection).success);
    EXPECT_EQ(f.inner->closeTcpConnectionCount(), 2);
    EXPECT_EQ(f.inner->closeConnectionCount(), 0);
    const Broker::Request expected{Broker::CloseConnectionRequest{.target = TARGET, .socketInode = 0, .tcpConnection = connection}};
    const std::vector<Broker::Request> requests = f.broker->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests[0], expected);
    // "Authenticate and retry" sends the same.
    EXPECT_EQ((brokerRequestFor({.kind = AuthorizedAction::Kind::CloseConnection, .target = TARGET, .tcpConnection = connection})),
              expected);
}

// #3069: one thread of another user's process, refused in process, is retried through the helper as a
// ThreadAction carrying the thread and the action; "Authenticate and retry" sends the same.
TEST(BrokeredProcessActionsTest, AThreadActionRefusedInProcessIsRetriedThroughTheHelper)
{
    Fixture f(/*connected=*/true);
    EXPECT_TRUE(f.actions->actOnThread(TARGET, 41, 500, ThreadAction::Suspend).success);
    EXPECT_TRUE(f.broker->requests().empty()) << "done in process: the helper isn't asked";

    f.inner->setThreadActionResult(ProcessActionResult::denied("Access denied: can't end thread 42 of process 4242"));
    EXPECT_TRUE(f.actions->actOnThread(TARGET, 42, 501, ThreadAction::End).success);
    EXPECT_EQ(f.inner->threadActionCount(), 2);
    const std::vector<Broker::Request> requests = f.broker->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(
        requests[0],
        (Broker::Request{Broker::ThreadActionRequest{.target = TARGET, .tid = 42, .threadCreateTime = 501, .action = ThreadAction::End}}));

    // A plain refusal (a critical process's thread) isn't one an administrator could do: never retried.
    f.inner->setThreadActionResult(ProcessActionResult::error("critical"));
    EXPECT_FALSE(f.actions->actOnThread(TARGET, 43, 502, ThreadAction::Suspend).success);
    EXPECT_EQ(f.broker->requests().size(), 1U);

    EXPECT_EQ(
        (brokerRequestFor({.kind = AuthorizedAction::Kind::ThreadAction,
                           .target = TARGET,
                           .tid = 9,
                           .threadCreateTime = 8,
                           .threadAction = ThreadAction::Resume})),
        (Broker::Request{Broker::ThreadActionRequest{.target = TARGET, .tid = 9, .threadCreateTime = 8, .action = ThreadAction::Resume}}));
}

// #3071: "Authenticate and retry" after a refused Efficiency mode starts the helper and sends it the change.
TEST(BrokeredProcessActionsTest, RunAuthorizedTurnsEfficiencyModeOnThroughTheHelper)
{
    const Fixture f(/*connected=*/false);
    const ProcessActionResult result = f.actions->runAuthorized(
        {.kind = AuthorizedAction::Kind::SetEfficiencyMode, .target = TARGET, .efficiencyMode = true}, std::stop_token{});
    EXPECT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(f.broker->connectCount(), 1);
    const std::vector<Broker::Request> requests = f.broker->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front(), (Broker::Request{Broker::EfficiencyModeRequest{.target = TARGET, .on = true}}));
    EXPECT_EQ(f.inner->efficiencyModeCount(), 0);
}

// #3580: a scheduling policy is Linux's, and no helper sets one since protocol v23: setSchedPolicy() is the in-process
// actions', a permission refusal included, and runAuthorized() refuses it without a prompt.
TEST(BrokeredProcessActionsTest, ASchedulingPolicyNeverReachesTheHelper)
{
    const Fixture f(/*connected=*/true);
    EXPECT_TRUE(f.actions->setSchedPolicy(TARGET, SchedPolicy::Batch, 0, Platform::RealtimeThreads::Keep).success);
    f.inner->setSchedPolicyResult(ProcessActionResult::denied("Permission denied: process 4242 belongs to another user"));
    const ProcessActionResult refused = f.actions->setSchedPolicy(TARGET, SchedPolicy::RoundRobin, 20, Platform::RealtimeThreads::Change);
    EXPECT_TRUE(refused.permissionDenied) << "the refusal stands";
    EXPECT_EQ(f.inner->setSchedPolicyCount(), 2);
    EXPECT_EQ(f.inner->lastRtPriority(), 20);
    EXPECT_EQ(f.inner->lastRealtimeThreads(), Platform::RealtimeThreads::Change);
    EXPECT_TRUE(f.broker->requests().empty());

    const Fixture fresh(/*connected=*/false);
    const ProcessActionResult authorized = fresh.actions->runAuthorized(
        {.kind = AuthorizedAction::Kind::SetSchedPolicy, .target = TARGET, .schedPolicy = SchedPolicy::Normal}, std::stop_token{});
    EXPECT_FALSE(authorized.success);
    EXPECT_EQ(authorized.errorMessage, "Setting a scheduling policy isn't available through the administrator helper");
    EXPECT_EQ(fresh.broker->connectCount(), 0) << "no prompt for what the helper can't do";
    EXPECT_TRUE(fresh.broker->requests().empty());
    EXPECT_EQ(fresh.inner->setSchedPolicyCount(), 0);

    // The capability is the inner actions', passed through.
    f.inner->setCapabilities({.canSetSchedPolicy = true});
    EXPECT_TRUE(f.actions->actionCapabilities().canSetSchedPolicy);
    f.inner->setCapabilities({});
    EXPECT_FALSE(f.actions->actionCapabilities().canSetSchedPolicy);
}

TEST(BrokeredProcessActionsTest, AnAffinityTheProcessAcceptsNeverReachesTheHelper)
{
    const Fixture f(/*connected=*/true);
    const CpuAffinity cpus = CpuAffinity::fromMask(0b101);
    EXPECT_TRUE(f.actions->setAffinity(TARGET, cpus).success);
    EXPECT_EQ(f.inner->setAffinityCount(), 1);
    EXPECT_EQ(f.inner->lastAffinity(), cpus);
    EXPECT_TRUE(f.broker->requests().empty());
}

TEST(BrokeredProcessActionsTest, AnUnknownActionKindAsksForTheGentlestSignal)
{
    // Every Kind maps to its own request; one outside the enum (a corrupt value) falls back to Terminate
    // rather than to anything more drastic.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) - the out-of-range value is the point
    const AuthorizedAction corrupt{.kind = static_cast<AuthorizedAction::Kind>(0xEE), .target = TARGET};
    EXPECT_EQ(brokerRequestFor(corrupt), (Broker::Request{Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Terminate}}));
}

TEST(BrokeredProcessActionsTest, WithoutAHelperRunAuthorizedRefusesAndNothingIsRetried)
{
    auto owned = std::make_unique<TestMocks::MockProcessActions>();
    TestMocks::MockProcessActions* const inner = owned.get();
    inner->setKillResult(ProcessActionResult::denied("Permission denied"));
    BrokeredProcessActions actions(std::move(owned), nullptr);

    EXPECT_FALSE(actions.actionCapabilities().canAuthorizeInteractively);
    const ProcessActionResult killed = actions.kill(TARGET);
    EXPECT_TRUE(killed.permissionDenied); // returned as it is: no helper to retry through
    const ProcessActionResult authorized =
        actions.runAuthorized({.kind = AuthorizedAction::Kind::Kill, .target = TARGET}, std::stop_token{});
    EXPECT_FALSE(authorized.success);
    EXPECT_EQ(authorized.errorMessage, "Acting as an administrator isn't available");
    EXPECT_EQ(inner->runAuthorizedCount(), 0); // the decorator's own refusal, not the inner one's
}

TEST(BrokeredProcessActionsTest, TraceDumpAndFolderCallsAreTheInProcessOnes)
{
    Fixture f(/*connected=*/true);
    f.inner->setSyscallTraceResult(ProcessActionResult::error("strace is not installed"));
    f.inner->setDumpResult(ProcessDump{.file = "/tmp/x.core", .sizeBytes = 42, .note = {}, .fromSnapshot = false, .liveReason = {}});
    f.inner->setDumpSpace({.freeBytes = 1000U, .processBytes = 10U});
    f.inner->setFolderChoice(std::optional<std::filesystem::path>{"/tmp"});

    EXPECT_EQ(f.actions->launchSyscallTrace(TARGET).errorMessage, "strace is not installed");
    EXPECT_EQ(f.inner->syscallTraceCount(), 1);

    const ProcessDumpResult dump = f.actions->createDump(TARGET, "/tmp", std::stop_token{});
    ASSERT_TRUE(dump.has_value());
    EXPECT_EQ(dump->sizeBytes, 42U);
    EXPECT_EQ(f.inner->lastDumpDirectory(), std::filesystem::path("/tmp"));

    const DumpSpace space = f.actions->dumpSpace(TARGET, "/var");
    EXPECT_EQ(space.freeBytes, std::optional<std::uint64_t>{1000U});
    EXPECT_EQ(f.inner->lastSpaceDirectory(), std::filesystem::path("/var"));

    int owner = 0;
    const FolderChoice choice = f.actions->chooseDumpFolder("/home", &owner);
    ASSERT_TRUE(choice.has_value());
    EXPECT_EQ(choice->value_or(std::filesystem::path{}), std::filesystem::path("/tmp"));
    EXPECT_EQ(f.inner->lastFolderInitial(), std::filesystem::path("/home"));
    EXPECT_EQ(f.inner->lastFolderOwner(), &owner);

    // A wait chain read (#3084) is the in-process one too, refusal and all: the helper serves none.
    WaitChainReadResult denied;
    denied.status = WaitChainReadStatus::PermissionDenied;
    f.inner->setWaitChainResult(denied);
    EXPECT_EQ(f.actions->readWaitChains(TARGET, std::stop_token{}).status, WaitChainReadStatus::PermissionDenied);
    EXPECT_EQ(f.inner->waitChainCount(), 1);
    EXPECT_EQ(f.inner->lastTarget(), TARGET);

    EXPECT_TRUE(f.broker->requests().empty()); // none of these goes through the helper
}

/// Process actions that implement only what IProcessActions requires, so every optional call takes the
/// interface's default.
class RequiredOnlyActions final : public IProcessActions
{
  public:
    [[nodiscard]] ProcessActionCapabilities actionCapabilities() const override
    {
        return {};
    }
    [[nodiscard]] ProcessActionResult terminate(const ProcessTarget& /*target*/) override
    {
        return ProcessActionResult::ok();
    }
    [[nodiscard]] ProcessActionResult kill(const ProcessTarget& /*target*/) override
    {
        return ProcessActionResult::ok();
    }
    [[nodiscard]] ProcessActionResult stop(const ProcessTarget& /*target*/) override
    {
        return ProcessActionResult::ok();
    }
    [[nodiscard]] ProcessActionResult resume(const ProcessTarget& /*target*/) override
    {
        return ProcessActionResult::ok();
    }
    [[nodiscard]] ProcessActionResult setPriority(const ProcessTarget& /*target*/, std::int32_t /*nice*/) override
    {
        return ProcessActionResult::ok();
    }
    [[nodiscard]] ProcessActionResult
    setIoPriority(const ProcessTarget& /*target*/, IoPriorityClass /*ioClass*/, std::int32_t /*level*/) override
    {
        return ProcessActionResult::ok();
    }
    [[nodiscard]] IoPriorityReadResult getIoPriority(const ProcessTarget& /*target*/) override
    {
        return std::unexpected(std::string("not read"));
    }
};

TEST(BrokeredProcessActionsTest, OverActionsWithOnlyTheDefaultsTheDefaultsShowThrough)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(true);
    BrokeredProcessActions actions(std::make_unique<RequiredOnlyActions>(), broker);

    const ProcessActionResult affinity = actions.setAffinity(TARGET, CpuAffinity::fromMask(1));
    EXPECT_FALSE(affinity.success);
    EXPECT_EQ(affinity.errorMessage, "Setting CPU affinity isn't available on this system");
    EXPECT_FALSE(affinity.permissionDenied);

    // A scheduling policy (#3086): not offered, refused as unavailable, never retried through the helper.
    EXPECT_FALSE(actions.actionCapabilities().canSetSchedPolicy);
    const ProcessActionResult policy = actions.setSchedPolicy(TARGET, SchedPolicy::Batch, 0, Platform::RealtimeThreads::Keep);
    EXPECT_FALSE(policy.success);
    EXPECT_EQ(policy.errorMessage, "Setting the scheduling policy isn't available on this system");
    EXPECT_FALSE(policy.permissionDenied);

    // Efficiency mode (#3071): not offered, and refused as unavailable, never retried through the helper.
    EXPECT_FALSE(actions.actionCapabilities().canSetEfficiencyMode);
    const ProcessActionResult efficiency = actions.setEfficiencyMode(TARGET, true);
    EXPECT_FALSE(efficiency.success);
    EXPECT_EQ(efficiency.errorMessage, "Efficiency mode isn't available on this system");
    EXPECT_FALSE(efficiency.permissionDenied);

    const ProcessDumpResult dump = actions.createDump(TARGET, "/tmp", std::stop_token{});
    ASSERT_FALSE(dump.has_value());
    EXPECT_EQ(dump.error(), DUMP_UNSUPPORTED_MESSAGE);

    const DumpSpace space = actions.dumpSpace(TARGET, "/tmp");
    EXPECT_FALSE(space.freeBytes.has_value());
    EXPECT_FALSE(space.processBytes.has_value());

    const FolderChoice choice = actions.chooseDumpFolder("/tmp", nullptr);
    ASSERT_FALSE(choice.has_value());
    EXPECT_EQ(choice.error(), FOLDER_CHOICE_UNSUPPORTED_MESSAGE);

    const CpuAffinityLayout layout = actions.affinityLayout();
    EXPECT_TRUE(layout.groupSizes.empty());

    // Analyze wait chain (#3084): not offered, and unsupported, never asked of the helper.
    EXPECT_FALSE(actions.actionCapabilities().canAnalyzeWaitChain);
    const WaitChainReadResult chains = actions.readWaitChains(TARGET, std::stop_token{});
    EXPECT_EQ(chains.status, WaitChainReadStatus::Unsupported);
    EXPECT_EQ(chains.detail, WAIT_CHAIN_UNSUPPORTED_MESSAGE);
    EXPECT_TRUE(chains.threads.empty());

    RequiredOnlyActions plain;
    const ProcessActionResult authorized = plain.runAuthorized({.kind = AuthorizedAction::Kind::Kill, .target = TARGET}, std::stop_token{});
    EXPECT_FALSE(authorized.success);
    EXPECT_EQ(authorized.errorMessage, "Acting as an administrator isn't available");
    EXPECT_TRUE(broker->requests().empty());
}

// --- The readers' factories, and their helper failures (#2417) ---------------------------------------

/// A broker whose every call fails with @p reason, as a broken channel does.
[[nodiscard]] std::shared_ptr<Mocks::FakeBroker> failingBroker(std::string reason)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(true);
    broker->setAnswer([reason = std::move(reason)](const Broker::Request&)
                      { return std::expected<Broker::Response, std::string>{std::unexpect, reason}; });
    return broker;
}

/// A broker that answers every call with an ActionResult, the wrong kind for any read.
[[nodiscard]] std::shared_ptr<Mocks::FakeBroker> confusedBroker()
{
    return std::make_shared<Mocks::FakeBroker>(true); // FakeBroker's default answer is an ActionResult
}

TEST(BrokeredEnvironmentReaderTest, TheFactoryWrapsOnlyAReaderThatCanRead)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(false);
    EXPECT_EQ(withPrivilegedBroker(std::unique_ptr<IProcessEnvironmentReader>{}, broker), nullptr);

    auto unable = std::make_unique<TestMocks::MockProcessEnvironmentReader>();
    unable->setHasEnvironment(false);
    const IProcessEnvironmentReader* const rawUnable = unable.get();
    EXPECT_EQ(withPrivilegedBroker(std::unique_ptr<IProcessEnvironmentReader>(std::move(unable)), broker).get(), rawUnable);

    auto able = std::make_unique<TestMocks::MockProcessEnvironmentReader>();
    const IProcessEnvironmentReader* const rawAble = able.get();
    const std::unique_ptr<IProcessEnvironmentReader> wrapped =
        withPrivilegedBroker(std::unique_ptr<IProcessEnvironmentReader>(std::move(able)), broker);
    EXPECT_NE(wrapped.get(), rawAble);
    EXPECT_TRUE(wrapped->hasEnvironment());
    EXPECT_TRUE(wrapped->canReadAuthorized());
}

TEST(BrokeredEnvironmentReaderTest, WithoutABrokerReadAuthorizedIsAnOrdinaryRead)
{
    auto owned = std::make_unique<TestMocks::MockProcessEnvironmentReader>();
    TestMocks::MockProcessEnvironmentReader* const inner = owned.get();
    inner->setResult({.status = EnvironmentReadStatus::PermissionDenied, .variables = {}});
    BrokeredEnvironmentReader reader(std::move(owned), nullptr);
    EXPECT_FALSE(reader.canReadAuthorized());
    EXPECT_EQ(reader.readAuthorized(TARGET, std::stop_token{}).status, EnvironmentReadStatus::PermissionDenied);
    EXPECT_EQ(inner->readCount(), 1);
}

TEST(BrokeredEnvironmentReaderTest, AHelperThatCantAnswerFailsTheRead)
{
    const auto readThrough = [](std::shared_ptr<Mocks::FakeBroker> broker)
    {
        auto owned = std::make_unique<TestMocks::MockProcessEnvironmentReader>();
        owned->setResult({.status = EnvironmentReadStatus::PermissionDenied, .variables = {}});
        BrokeredEnvironmentReader reader(std::move(owned), std::move(broker));
        return reader.readEnvironment(TARGET).status;
    };
    EXPECT_EQ(readThrough(failingBroker("the channel broke")), EnvironmentReadStatus::Failed);
    EXPECT_EQ(readThrough(confusedBroker()), EnvironmentReadStatus::Failed);
}

TEST(BrokeredModulesReaderTest, TheFactoryWrapsOnlyAReaderThatCanRead)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(false);
    EXPECT_EQ(withPrivilegedBroker(std::unique_ptr<IProcessModulesReader>{}, broker), nullptr);
    auto unsupported = std::make_unique<UnsupportedProcessModulesReader>();
    const IProcessModulesReader* const rawUnsupported = unsupported.get();
    EXPECT_EQ(withPrivilegedBroker(std::unique_ptr<IProcessModulesReader>(std::move(unsupported)), broker).get(), rawUnsupported);

    const std::unique_ptr<IProcessModulesReader> wrapped =
        withPrivilegedBroker(std::unique_ptr<IProcessModulesReader>(std::make_unique<TestMocks::MockProcessModulesReader>()), broker);
    EXPECT_TRUE(wrapped->hasModules());
    EXPECT_TRUE(wrapped->canReadAuthorized());
}

TEST(BrokeredModulesReaderTest, WithoutABrokerOrAPromptTheRefusalStands)
{
    auto owned = std::make_unique<TestMocks::MockProcessModulesReader>();
    TestMocks::MockProcessModulesReader* const inner = owned.get();
    inner->setResult({.status = ModulesReadStatus::PermissionDenied, .modules = {}, .detail = {}});
    BrokeredModulesReader alone(std::move(owned), nullptr);
    EXPECT_FALSE(alone.canReadAuthorized());
    EXPECT_EQ(alone.readAuthorized(TARGET, std::stop_token{}).status, ModulesReadStatus::PermissionDenied);
    EXPECT_EQ(inner->readCount(), 1); // an ordinary read

    auto broker = std::make_shared<Mocks::FakeBroker>(false);
    broker->setConnectFailure("the password prompt was dismissed");
    BrokeredModulesReader dismissed(std::make_unique<TestMocks::MockProcessModulesReader>(), broker);
    EXPECT_EQ(dismissed.readAuthorized(TARGET, std::stop_token{}).status, ModulesReadStatus::PermissionDenied);
    EXPECT_TRUE(broker->requests().empty());
}

TEST(BrokeredModulesReaderTest, AHelperThatCantAnswerSaysWhy)
{
    auto refused = std::make_unique<TestMocks::MockProcessModulesReader>();
    refused->setResult({.status = ModulesReadStatus::PermissionDenied, .modules = {}, .detail = {}});
    BrokeredModulesReader broken(std::move(refused), failingBroker("the channel broke"));
    const ModulesReadResult failed = broken.readModules(TARGET);
    EXPECT_EQ(failed.status, ModulesReadStatus::Failed);
    EXPECT_EQ(failed.detail, "The administrator helper couldn't be asked: the channel broke");

    auto refusedAgain = std::make_unique<TestMocks::MockProcessModulesReader>();
    refusedAgain->setResult({.status = ModulesReadStatus::PermissionDenied, .modules = {}, .detail = {}});
    BrokeredModulesReader confused(std::move(refusedAgain), confusedBroker());
    const ModulesReadResult unexpected = confused.readModules(TARGET);
    EXPECT_EQ(unexpected.status, ModulesReadStatus::Failed);
    EXPECT_EQ(unexpected.detail, "The administrator helper gave an unexpected answer");
}

TEST(BrokeredOpenFilesReaderTest, TheFactoryWrapsOnlyAReaderThatCanRead)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(false);
    EXPECT_EQ(withPrivilegedBroker(std::unique_ptr<IProcessOpenFilesReader>{}, broker), nullptr);
    auto unsupported = std::make_unique<UnsupportedProcessOpenFilesReader>();
    const IProcessOpenFilesReader* const rawUnsupported = unsupported.get();
    EXPECT_EQ(withPrivilegedBroker(std::unique_ptr<IProcessOpenFilesReader>(std::move(unsupported)), broker).get(), rawUnsupported);

    const std::unique_ptr<IProcessOpenFilesReader> wrapped =
        withPrivilegedBroker(std::unique_ptr<IProcessOpenFilesReader>(std::make_unique<TestMocks::MockProcessOpenFilesReader>()), broker);
    EXPECT_TRUE(wrapped->hasOpenFiles());
    EXPECT_TRUE(wrapped->canReadAuthorized());
    EXPECT_EQ(wrapped->handlesScanned(), 0U); // the inner reader's count, which the mock leaves at the default
}

TEST(BrokeredOpenFilesReaderTest, AnInProcessReadAndAReadWithoutABrokerNeverReachTheHelper)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(true);
    auto ok = std::make_unique<TestMocks::MockProcessOpenFilesReader>();
    OpenFilesReadResult read;
    read.status = OpenFilesReadStatus::Ok;
    ok->setResult(read);
    BrokeredOpenFilesReader reader(std::move(ok), broker);
    EXPECT_EQ(reader.readOpenFiles(TARGET).status, OpenFilesReadStatus::Ok);
    EXPECT_TRUE(broker->requests().empty());

    auto refused = std::make_unique<TestMocks::MockProcessOpenFilesReader>();
    const TestMocks::MockProcessOpenFilesReader* const inner = refused.get();
    OpenFilesReadResult denied;
    denied.status = OpenFilesReadStatus::PermissionDenied;
    refused->setResult(denied);
    BrokeredOpenFilesReader alone(std::move(refused), nullptr);
    EXPECT_FALSE(alone.canReadAuthorized());
    EXPECT_EQ(alone.readAuthorized(TARGET, std::stop_token{}).status, OpenFilesReadStatus::PermissionDenied);
    EXPECT_EQ(inner->readCount(), 1);
}

// #3357: a search's batch reaches the reader inside, so its in-process reads come from one walk of the handle table.
TEST(BrokeredOpenFilesReaderTest, ABatchIsTheInnerReaders)
{
    auto inner = std::make_unique<TestMocks::MockProcessOpenFilesReader>();
    const TestMocks::MockProcessOpenFilesReader* const raw = inner.get();
    BrokeredOpenFilesReader reader(std::move(inner), nullptr);
    reader.beginBatch();
    EXPECT_EQ(raw->batchesBegun(), 1);
    EXPECT_EQ(raw->batchesEnded(), 0);
    reader.endBatch();
    EXPECT_EQ(raw->batchesEnded(), 1);
}

TEST(BrokeredOpenFilesReaderTest, AHelperThatCantAnswerSaysWhy)
{
    OpenFilesReadResult denied;
    denied.status = OpenFilesReadStatus::PermissionDenied;

    auto refused = std::make_unique<TestMocks::MockProcessOpenFilesReader>();
    refused->setResult(denied);
    BrokeredOpenFilesReader broken(std::move(refused), failingBroker("timed out"));
    const OpenFilesReadResult failed = broken.readOpenFiles(TARGET);
    EXPECT_EQ(failed.status, OpenFilesReadStatus::Failed);
    EXPECT_EQ(failed.detail, "The administrator helper couldn't be asked: timed out");

    auto refusedAgain = std::make_unique<TestMocks::MockProcessOpenFilesReader>();
    refusedAgain->setResult(denied);
    BrokeredOpenFilesReader confused(std::move(refusedAgain), confusedBroker());
    const OpenFilesReadResult unexpected = confused.readOpenFiles(TARGET);
    EXPECT_EQ(unexpected.status, OpenFilesReadStatus::Failed);
    EXPECT_EQ(unexpected.detail, "The administrator helper gave an unexpected answer");
}

TEST(BrokeredConnectionsReaderTest, TheFactoryWrapsOnlyAReaderThatCanRead)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(false);
    EXPECT_EQ(withPrivilegedBroker(std::unique_ptr<IProcessConnectionsReader>{}, broker), nullptr);
    auto unsupported = std::make_unique<UnsupportedProcessConnectionsReader>();
    const IProcessConnectionsReader* const rawUnsupported = unsupported.get();
    EXPECT_EQ(withPrivilegedBroker(std::unique_ptr<IProcessConnectionsReader>(std::move(unsupported)), broker).get(), rawUnsupported);

    const std::unique_ptr<IProcessConnectionsReader> wrapped = withPrivilegedBroker(
        std::unique_ptr<IProcessConnectionsReader>(std::make_unique<TestMocks::MockProcessConnectionsReader>()), broker);
    EXPECT_TRUE(wrapped->hasConnections());
    EXPECT_TRUE(wrapped->canReadAuthorized());
}

TEST(BrokeredConnectionsReaderTest, WithoutABrokerOrAPromptTheRefusalStands)
{
    auto owned = std::make_unique<TestMocks::MockProcessConnectionsReader>();
    TestMocks::MockProcessConnectionsReader* const inner = owned.get();
    inner->setResult({.status = ConnectionsReadStatus::PermissionDenied, .connections = {}, .detail = {}});
    BrokeredConnectionsReader alone(std::move(owned), nullptr);
    EXPECT_FALSE(alone.canReadAuthorized());
    EXPECT_EQ(alone.readAuthorized(TARGET, std::stop_token{}).status, ConnectionsReadStatus::PermissionDenied);
    EXPECT_EQ(inner->readCount(), 1);

    auto broker = std::make_shared<Mocks::FakeBroker>(false);
    broker->setConnectFailure("the password prompt was dismissed");
    BrokeredConnectionsReader dismissed(std::make_unique<TestMocks::MockProcessConnectionsReader>(), broker);
    EXPECT_EQ(dismissed.readAuthorized(TARGET, std::stop_token{}).status, ConnectionsReadStatus::PermissionDenied);
    EXPECT_TRUE(broker->requests().empty());
}

TEST(BrokeredConnectionsReaderTest, AHelperThatCantAnswerSaysWhy)
{
    const ConnectionsReadResult denied{.status = ConnectionsReadStatus::PermissionDenied, .connections = {}, .detail = {}};

    auto refused = std::make_unique<TestMocks::MockProcessConnectionsReader>();
    refused->setResult(denied);
    BrokeredConnectionsReader broken(std::move(refused), failingBroker("the channel broke"));
    const ConnectionsReadResult failed = broken.readConnections(TARGET);
    EXPECT_EQ(failed.status, ConnectionsReadStatus::Failed);
    EXPECT_EQ(failed.detail, "The administrator helper couldn't be asked: the channel broke");

    auto refusedAgain = std::make_unique<TestMocks::MockProcessConnectionsReader>();
    refusedAgain->setResult(denied);
    BrokeredConnectionsReader confused(std::move(refusedAgain), confusedBroker());
    const ConnectionsReadResult unexpected = confused.readConnections(TARGET);
    EXPECT_EQ(unexpected.status, ConnectionsReadStatus::Failed);
    EXPECT_EQ(unexpected.detail, "The administrator helper gave an unexpected answer");
}

/// Readers that implement only what their interfaces require, so readAuthorized(), canReadAuthorized()
/// and handlesScanned() take the interfaces' defaults.
class RequiredOnlyEnvironmentReader final : public IProcessEnvironmentReader
{
  public:
    [[nodiscard]] bool hasEnvironment() const override
    {
        return true;
    }
    [[nodiscard]] EnvironmentReadResult readEnvironment(const ProcessTarget& /*target*/) override
    {
        ++reads;
        return {.status = EnvironmentReadStatus::PermissionDenied, .variables = {}};
    }
    int reads = 0;
};

class RequiredOnlyModulesReader final : public IProcessModulesReader
{
  public:
    [[nodiscard]] bool hasModules() const override
    {
        return true;
    }
    [[nodiscard]] ModulesReadResult readModules(const ProcessTarget& /*target*/) override
    {
        ++reads;
        return {.status = ModulesReadStatus::PermissionDenied, .modules = {}, .detail = {}};
    }
    int reads = 0;
};

class RequiredOnlyOpenFilesReader final : public IProcessOpenFilesReader
{
  public:
    [[nodiscard]] bool hasOpenFiles() const override
    {
        return true;
    }
    [[nodiscard]] OpenFilesReadResult readOpenFiles(const ProcessTarget& /*target*/) override
    {
        ++reads;
        OpenFilesReadResult result;
        result.status = OpenFilesReadStatus::PermissionDenied;
        return result;
    }
    int reads = 0;
};

class RequiredOnlyConnectionsReader final : public IProcessConnectionsReader
{
  public:
    [[nodiscard]] bool hasConnections() const override
    {
        return true;
    }
    [[nodiscard]] ConnectionsReadResult readConnections(const ProcessTarget& /*target*/) override
    {
        ++reads;
        return {.status = ConnectionsReadStatus::PermissionDenied, .connections = {}, .detail = {}};
    }
    int reads = 0;
};

TEST(ProcessReaderDefaultsTest, AReaderWithoutAnAuthorizedPathOffersNoneAndReadsAsUsual)
{
    RequiredOnlyEnvironmentReader environment;
    EXPECT_FALSE(environment.canReadAuthorized());
    EXPECT_EQ(environment.readAuthorized(TARGET, std::stop_token{}).status, EnvironmentReadStatus::PermissionDenied);
    EXPECT_EQ(environment.reads, 1);

    RequiredOnlyModulesReader modules;
    EXPECT_FALSE(modules.canReadAuthorized());
    EXPECT_EQ(modules.readAuthorized(TARGET, std::stop_token{}).status, ModulesReadStatus::PermissionDenied);
    EXPECT_EQ(modules.reads, 1);

    RequiredOnlyOpenFilesReader files;
    EXPECT_FALSE(files.canReadAuthorized());
    EXPECT_EQ(files.readAuthorized(TARGET, std::stop_token{}).status, OpenFilesReadStatus::PermissionDenied);
    EXPECT_EQ(files.reads, 1);
    EXPECT_EQ(files.handlesScanned(), 0U);
    // No batch of its own (#3357): reads inside one read as usual.
    files.beginBatch();
    EXPECT_EQ(files.readOpenFiles(TARGET).status, OpenFilesReadStatus::PermissionDenied);
    files.endBatch();
    EXPECT_EQ(files.reads, 2);

    RequiredOnlyConnectionsReader connections;
    EXPECT_FALSE(connections.canReadAuthorized());
    EXPECT_EQ(connections.readAuthorized(TARGET, std::stop_token{}).status, ConnectionsReadStatus::PermissionDenied);
    EXPECT_EQ(connections.reads, 1);
}

TEST(ProcessReaderDefaultsTest, TheUnsupportedReadersReadNothing)
{
    UnsupportedProcessModulesReader modules;
    EXPECT_FALSE(modules.hasModules());
    EXPECT_EQ(modules.readModules(TARGET).status, ModulesReadStatus::Unsupported);

    UnsupportedProcessOpenFilesReader files;
    EXPECT_FALSE(files.hasOpenFiles());
    EXPECT_TRUE(files.readOpenFiles(TARGET).files.empty());

    UnsupportedProcessConnectionsReader connections;
    EXPECT_FALSE(connections.hasConnections());
    EXPECT_EQ(connections.readConnections(TARGET).status, ConnectionsReadStatus::Unsupported);
}

// --- BrokeredThreadsReader (#3069) --------------------------------------------------------------------

/// A threads reader answering @p result every time, counting its reads.
class ScriptedThreadsReader final : public IProcessThreadsReader
{
  public:
    explicit ScriptedThreadsReader(ThreadsReadResult result, bool has = true) : m_Result(std::move(result)), m_Has(has)
    {}
    [[nodiscard]] bool hasThreads() const override
    {
        return m_Has;
    }
    [[nodiscard]] ThreadsReadResult readThreads(const ProcessTarget& /*target*/) override
    {
        ++m_Reads;
        return m_Result;
    }
    int m_Reads = 0;

  private:
    ThreadsReadResult m_Result;
    bool m_Has = true;
};

[[nodiscard]] ThreadsReadResult threadsWith(ThreadsReadStatus status)
{
    return {.status = status, .threads = {}, .detail = {}};
}

TEST(BrokeredThreadsReaderTest, ARefusedReadGoesThroughARunningHelperAndOtherwiseWaitsForTheUi)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(true);
    broker->setAnswer([](const Broker::Request&)
                      { return Broker::Response{Broker::ThreadsReply{.result = threadsWith(ThreadsReadStatus::Ok)}}; });
    BrokeredThreadsReader reader(std::make_unique<ScriptedThreadsReader>(threadsWith(ThreadsReadStatus::PermissionDenied)), broker);
    EXPECT_TRUE(reader.hasThreads());
    EXPECT_EQ(reader.readThreads(TARGET).status, ThreadsReadStatus::Ok);
    ASSERT_EQ(broker->requests().size(), 1U);
    EXPECT_EQ(broker->requests().front(), (Broker::Request{Broker::ThreadsRequest{.target = TARGET}}));

    broker->setConnected(false);
    EXPECT_EQ(reader.readThreads(TARGET).status, ThreadsReadStatus::PermissionDenied);
    EXPECT_TRUE(reader.canReadAuthorized());
    EXPECT_EQ(reader.readAuthorized(TARGET, std::stop_token{}).status, ThreadsReadStatus::Ok);
    EXPECT_EQ(broker->connectCount(), 1);

    // A read the reader makes itself goes nowhere near the helper.
    BrokeredThreadsReader own(std::make_unique<ScriptedThreadsReader>(threadsWith(ThreadsReadStatus::Ok)), broker);
    EXPECT_EQ(own.readThreads(TARGET).status, ThreadsReadStatus::Ok);
    EXPECT_EQ(broker->requests().size(), 2U);
}

TEST(BrokeredThreadsReaderTest, TheFactoryWrapsOnlyAReaderThatCanReadAndAWithoutHelperRefusalStands)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(false);
    EXPECT_EQ(withPrivilegedBroker(std::unique_ptr<IProcessThreadsReader>{}, broker), nullptr);
    auto unsupported = std::make_unique<UnsupportedProcessThreadsReader>();
    const IProcessThreadsReader* const rawUnsupported = unsupported.get();
    EXPECT_EQ(withPrivilegedBroker(std::unique_ptr<IProcessThreadsReader>(std::move(unsupported)), broker).get(), rawUnsupported);
    EXPECT_TRUE(
        withPrivilegedBroker(
            std::unique_ptr<IProcessThreadsReader>(std::make_unique<ScriptedThreadsReader>(threadsWith(ThreadsReadStatus::Ok))), broker)
            ->canReadAuthorized());

    auto owned = std::make_unique<ScriptedThreadsReader>(threadsWith(ThreadsReadStatus::PermissionDenied));
    ScriptedThreadsReader* const inner = owned.get();
    BrokeredThreadsReader alone(std::move(owned), nullptr);
    EXPECT_FALSE(alone.canReadAuthorized());
    EXPECT_EQ(alone.readAuthorized(TARGET, std::stop_token{}).status, ThreadsReadStatus::PermissionDenied);
    EXPECT_EQ(alone.readThreads(TARGET).status, ThreadsReadStatus::PermissionDenied);
    EXPECT_EQ(inner->m_Reads, 2);

    broker->setConnectFailure("the password prompt was dismissed");
    BrokeredThreadsReader dismissed(std::make_unique<ScriptedThreadsReader>(threadsWith(ThreadsReadStatus::PermissionDenied)), broker);
    EXPECT_EQ(dismissed.readAuthorized(TARGET, std::stop_token{}).status, ThreadsReadStatus::PermissionDenied);
    EXPECT_TRUE(broker->requests().empty());
}

TEST(BrokeredThreadsReaderTest, AHelperThatCantAnswerSaysWhy)
{
    BrokeredThreadsReader broken(std::make_unique<ScriptedThreadsReader>(threadsWith(ThreadsReadStatus::PermissionDenied)),
                                 failingBroker("the channel broke"));
    const ThreadsReadResult failed = broken.readThreads(TARGET);
    EXPECT_EQ(failed.status, ThreadsReadStatus::Failed);
    EXPECT_TRUE(failed.detail.contains("the channel broke")) << failed.detail;

    auto odd = std::make_shared<Mocks::FakeBroker>(true);
    odd->setAnswer([](const Broker::Request&) { return Broker::Response{Broker::ErrorReply{.message = "no"}}; });
    BrokeredThreadsReader confused(std::make_unique<ScriptedThreadsReader>(threadsWith(ThreadsReadStatus::PermissionDenied)), odd);
    const ThreadsReadResult unexpected = confused.readThreads(TARGET);
    EXPECT_EQ(unexpected.status, ThreadsReadStatus::Failed);
    EXPECT_EQ(unexpected.detail, "The administrator helper gave an unexpected answer");
}

} // namespace
} // namespace Platform
