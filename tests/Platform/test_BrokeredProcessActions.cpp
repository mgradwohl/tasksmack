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
#include "Platform/IProcessProbe.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
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
}

TEST(BrokeredProcessActionsTest, TheRealtimeIoClassIsOfferedWhileTheHelperRuns)
{
    const Fixture f(/*connected=*/false);
    EXPECT_FALSE(f.actions->actionCapabilities().canSetRealtimeIoPriority);
    f.broker->setConnected(true);
    EXPECT_TRUE(f.actions->actionCapabilities().canSetRealtimeIoPriority);
}

TEST(BrokeredProcessActionsTest, ReadsAndOtherCallsAreTheInProcessOnes)
{
    const Fixture f(/*connected=*/true);
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

// --- BrokeredProcessProbe (#2309) ----------------------------------------------------------------------

TEST(BrokeredProcessProbeTest, ARefusedMemoryRollupIsReadThroughARunningHelper)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(true);
    broker->setAnswer(
        [](const Broker::Request&)
        { return Broker::Response{Broker::MemoryRollupReply{.result = MemoryRollup{.pssBytes = 7, .ussBytes = 5, .swapPssBytes = 0}}}; });
    auto owned = std::make_unique<TestMocks::MockProcessProbe>();
    owned->setMemoryRollupDenied(TARGET.pid);
    owned->setMemoryRollup(99, MemoryRollup{.pssBytes = 1, .ussBytes = 1, .swapPssBytes = 0});
    const std::unique_ptr<IProcessProbe> probe = withPrivilegedBroker(std::move(owned), broker);

    const MemoryRollupRead own = probe->readMemoryRollup(99, 1);
    EXPECT_TRUE(own.has_value());
    EXPECT_TRUE(broker->requests().empty()); // read in-process

    const MemoryRollupRead other = probe->readMemoryRollup(TARGET.pid, TARGET.startTimeTicks);
    ASSERT_TRUE(other.has_value());
    EXPECT_EQ(other.value_or(MemoryRollup{}).pssBytes, 7U);
    ASSERT_EQ(broker->requests().size(), 1U);
    EXPECT_EQ(broker->requests().front(), (Broker::Request{Broker::MemoryRollupRequest{.target = TARGET}}));
    EXPECT_EQ(broker->idleCallCount(), 1) << "the sampler's read never queues behind another call (#2369)";

    broker->setConnected(false); // no helper running: the refusal stands, and nothing prompts
    const MemoryRollupRead refused = probe->readMemoryRollup(TARGET.pid, TARGET.startTimeTicks);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), MemoryRollupFailure::PermissionDenied);
    EXPECT_EQ(broker->connectCount(), 0);
}

/// A process as the probe read it: @p readable says whether its FD count and I/O were.
[[nodiscard]] ProcessCounters countersOf(const ProcessTarget& target, bool readable)
{
    ProcessCounters counters;
    counters.pid = target.pid;
    counters.startTimeTicks = target.startTimeTicks;
    counters.handleCount = readable ? 10 : 0;
    counters.handleCountAvailable = readable;
    counters.ioCountersAvailable = readable;
    return counters;
}

TEST(BrokeredProcessProbeTest, RefusedFdCountsAndIoAreReadThroughARunningHelperInOneCall)
{
    const ProcessTarget mine{.pid = 99, .startTimeTicks = 1};
    const ProcessTarget gone{.pid = 98, .startTimeTicks = 2};
    auto broker = std::make_shared<Mocks::FakeBroker>(true);
    broker->setAnswer(
        [](const Broker::Request&)
        {
            return Broker::Response{Broker::ProcessCountersReply{
                .entries = {Broker::ProcessCountersEntry{
                    .target = TARGET, .handleCountRead = true, .handleCount = 57, .ioRead = true, .readBytes = 1000, .writeBytes = 2000}}}};
        });
    auto owned = std::make_unique<TestMocks::MockProcessProbe>();
    owned->setCounters({countersOf(mine, true), countersOf(TARGET, false), countersOf(gone, false)});
    BrokeredProcessProbe probe(std::move(owned), broker);

    const std::vector<ProcessCounters> counters = probe.enumerate();
    ASSERT_EQ(broker->requests().size(), 1U);
    EXPECT_EQ(broker->requests().front(), (Broker::Request{Broker::ProcessCountersRequest{.targets = {TARGET, gone}}}));
    EXPECT_EQ(broker->idleCallCount(), 1) << "the sampler never queues behind another call";
    ASSERT_EQ(counters.size(), 3U);
    EXPECT_EQ(counters[0].handleCount, 10) << "read in-process, left alone";
    EXPECT_TRUE(counters[1].handleCountAvailable);
    EXPECT_EQ(counters[1].handleCount, 57);
    EXPECT_TRUE(counters[1].ioCountersAvailable);
    EXPECT_EQ(counters[1].writeBytes, 2000U);
    EXPECT_FALSE(counters[2].handleCountAvailable) << "gone: no entry, still refused";

    broker->setConnected(false); // no helper: nothing asked, nothing prompts
    static_cast<void>(probe.enumerate());
    EXPECT_EQ(broker->requests().size(), 1U);
    EXPECT_EQ(broker->connectCount(), 0);
}

// #2728: another user's sockets, which the probe's inode map can't own without reading its fd links, are
// attributed from the helper's socket inodes, asked for at the map's cadence rather than every refresh.
TEST(BrokeredProcessProbeTest, TheHelpersSocketInodesAttributeConnectionsTheProbeLeftUnowned)
{
    constexpr std::uint64_t INODE = 4242;
    auto broker = std::make_shared<Mocks::FakeBroker>(true);
    broker->setAnswer(
        [](const Broker::Request&)
        {
            return Broker::Response{Broker::ProcessCountersReply{.entries = {Broker::ProcessCountersEntry{.target = TARGET,
                                                                                                          .handleCountRead = true,
                                                                                                          .handleCount = 5,
                                                                                                          .ioRead = false,
                                                                                                          .readBytes = 0,
                                                                                                          .writeBytes = 0,
                                                                                                          .socketsRead = true,
                                                                                                          .socketInodes = {INODE}}}}};
        });
    auto owned = std::make_unique<TestMocks::MockProcessProbe>();
    ProcessCapabilities caps;
    caps.hasNetworkCounters = true;
    owned->setCapabilities(caps);
    ProcessCounters listed = countersOf(TARGET, true); // its FDs listed and its I/O read, but not its links
    listed.networkCountersAvailable = false;
    owned->setCounters({listed});
    SocketTrafficReading traffic;
    traffic.sampleTimeNs = 1;
    traffic.sockets = {SocketTrafficSample{.key = INODE, .pid = 0, .bytesReceived = 10, .bytesSent = 20},
                       SocketTrafficSample{.key = INODE + 1, .pid = 0, .bytesReceived = 1, .bytesSent = 1}};
    owned->setSocketTraffic(traffic);
    BrokeredProcessProbe probe(std::move(owned), broker);

    EXPECT_TRUE(probe.readSocketTraffic().sockets.front().pid == 0) << "nothing from the helper yet";
    const std::vector<ProcessCounters> counters = probe.enumerate();
    ASSERT_EQ(broker->requests().size(), 1U);
    EXPECT_EQ(broker->requests().front(), (Broker::Request{Broker::ProcessCountersRequest{.targets = {TARGET}, .withSockets = true}}));
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_TRUE(counters.front().networkCountersAvailable);
    const SocketTrafficReading attributed = probe.readSocketTraffic();
    ASSERT_EQ(attributed.sockets.size(), 2U);
    EXPECT_EQ(attributed.sockets[0].pid, TARGET.pid);
    EXPECT_EQ(attributed.sockets[0].ownerStartTimeTicks, TARGET.startTimeTicks);
    EXPECT_EQ(attributed.sockets[1].pid, 0) << "not among the helper's inodes: still unowned";

    // The next refresh, before the map's cadence comes round: nothing else refused, so nothing asked, and
    // the last read's attribution stands.
    const std::vector<ProcessCounters> again = probe.enumerate();
    EXPECT_EQ(broker->requests().size(), 1U);
    ASSERT_EQ(again.size(), 1U);
    EXPECT_TRUE(again.front().networkCountersAvailable);
}

TEST(BrokeredProcessProbeTest, AHelperThatRefusesProcessCountersIsntAskedAgain)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(true);
    broker->setAnswer([](const Broker::Request&) { return Broker::Response{Broker::ErrorReply{.message = "not here"}}; });
    auto owned = std::make_unique<TestMocks::MockProcessProbe>();
    owned->setCounters({countersOf(TARGET, false)});
    BrokeredProcessProbe probe(std::move(owned), broker);
    static_cast<void>(probe.enumerate());
    static_cast<void>(probe.enumerate());
    EXPECT_EQ(broker->requests().size(), 1U) << "Windows' helper answers with an error: asked once";
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

// --- BrokeredProcessProbe's pass-through calls (#2417) --------------------------------------------------

/// A probe that answers each call with a figure of its own and records the settings it is given, so the
/// decorator is seen to pass each one on.
class RecordingProbe final : public IProcessProbe
{
  public:
    [[nodiscard]] std::vector<ProcessCounters> enumerate() override
    {
        ProcessCounters counters;
        counters.pid = 77;
        return {counters};
    }
    [[nodiscard]] ProcessCapabilities capabilities() const override
    {
        ProcessCapabilities caps;
        caps.hasMemoryRollup = true;
        return caps;
    }
    [[nodiscard]] std::uint64_t totalCpuTime() const override
    {
        return 1234;
    }
    [[nodiscard]] long ticksPerSecond() const override
    {
        return 250;
    }
    [[nodiscard]] std::uint64_t systemTotalMemory() const override
    {
        return 8192;
    }
    [[nodiscard]] std::optional<PackageEnergyReading> readPackageEnergy() const override
    {
        return PackageEnergyReading{.energyUj = 55U, .maxRangeUj = 100U, .busyCpuTicks = std::nullopt, .otherPackages = {}};
    }
    [[nodiscard]] SocketTrafficReading readSocketTraffic() const override
    {
        return {.sockets = {}, .sampleTimeNs = 999};
    }
    [[nodiscard]] MemoryRollupRead readMemoryRollup(std::int32_t /*pid*/, std::uint64_t /*startTimeTicks*/) const override
    {
        return std::unexpected(rollupFailure);
    }
    void setSocketStatsCacheTtl(std::chrono::milliseconds ttlMs) override
    {
        *lastTtl = ttlMs;
    }
    void setUdpTrafficCollector(const std::shared_ptr<IUdpTrafficCollector>& /*collector*/) override
    {
        ++*collectorSets;
    }

    MemoryRollupFailure rollupFailure = MemoryRollupFailure::PermissionDenied;
    // Shared, so the test still sees them once the decorator owns the probe.
    std::shared_ptr<std::chrono::milliseconds> lastTtl = std::make_shared<std::chrono::milliseconds>(0);
    std::shared_ptr<int> collectorSets = std::make_shared<int>(0);
};

TEST(BrokeredProcessProbeTest, EveryOtherCallIsTheInnerProbes)
{
    auto owned = std::make_unique<RecordingProbe>();
    const std::shared_ptr<std::chrono::milliseconds> lastTtl = owned->lastTtl;
    const std::shared_ptr<int> collectorSets = owned->collectorSets;
    const std::unique_ptr<IProcessProbe> probe = withPrivilegedBroker(std::move(owned), std::make_shared<Mocks::FakeBroker>(true));

    const std::vector<ProcessCounters> counters = probe->enumerate();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters.front().pid, 77);
    EXPECT_TRUE(probe->capabilities().hasMemoryRollup);
    EXPECT_EQ(probe->totalCpuTime(), 1234U);
    EXPECT_EQ(probe->ticksPerSecond(), 250);
    EXPECT_EQ(probe->systemTotalMemory(), 8192U);
    const std::optional<PackageEnergyReading> energy = probe->readPackageEnergy();
    ASSERT_TRUE(energy.has_value());
    EXPECT_EQ(energy.value_or(PackageEnergyReading{}).energyUj, std::optional<std::uint64_t>{55U});
    EXPECT_EQ(probe->readSocketTraffic().sampleTimeNs, 999U);

    probe->setSocketStatsCacheTtl(std::chrono::milliseconds{750});
    EXPECT_EQ(*lastTtl, std::chrono::milliseconds{750});
    probe->setUdpTrafficCollector(nullptr);
    EXPECT_EQ(*collectorSets, 1);
}

TEST(BrokeredProcessProbeTest, OnlyAPermissionRefusalGoesToTheHelperAndAFailedCallLeavesIt)
{
    auto unavailable = std::make_unique<RecordingProbe>();
    unavailable->rollupFailure = MemoryRollupFailure::Unavailable;
    auto broker = std::make_shared<Mocks::FakeBroker>(true);
    const BrokeredProcessProbe gone(std::move(unavailable), broker);
    const MemoryRollupRead absent = gone.readMemoryRollup(TARGET.pid, TARGET.startTimeTicks);
    ASSERT_FALSE(absent.has_value());
    EXPECT_EQ(absent.error(), MemoryRollupFailure::Unavailable);
    EXPECT_TRUE(broker->requests().empty()); // the helper can't read a process that isn't there

    const auto expectRefusalStands = [](const std::shared_ptr<Mocks::FakeBroker>& helper)
    {
        const BrokeredProcessProbe refused(std::make_unique<RecordingProbe>(), helper);
        const MemoryRollupRead read = refused.readMemoryRollup(TARGET.pid, TARGET.startTimeTicks);
        ASSERT_FALSE(read.has_value());
        EXPECT_EQ(read.error(), MemoryRollupFailure::PermissionDenied); // the refusal stands
        EXPECT_EQ(helper->idleCallCount(), 1);
    };
    expectRefusalStands(failingBroker("busy"));
    expectRefusalStands(confusedBroker());
}

TEST(BrokeredProcessProbeTest, TheFactoryWrapsOnlyAProbeThatReadsMemoryRollups)
{
    auto broker = std::make_shared<Mocks::FakeBroker>(true);
    EXPECT_EQ(withPrivilegedBroker(std::unique_ptr<IProcessProbe>{}, broker), nullptr);
    auto plain = std::make_unique<TestMocks::MockProcessProbe>(); // hasMemoryRollup is off by default
    const IProcessProbe* const raw = plain.get();
    EXPECT_EQ(withPrivilegedBroker(std::unique_ptr<IProcessProbe>(std::move(plain)), broker).get(), raw);
    auto alone = std::make_unique<RecordingProbe>();
    const IProcessProbe* const rawAlone = alone.get();
    EXPECT_EQ(withPrivilegedBroker(std::unique_ptr<IProcessProbe>(std::move(alone)), nullptr).get(), rawAlone);
}

} // namespace
} // namespace Platform
