// Tests for the privileged helper's request mapping (#2307, #2309, #2310), which both helpers share
// (#2517): handleRequest() onto IProcessActions and the readers, with mocks. The transports' session
// loops have their own tests: test_BrokerServer.cpp (Linux) and test_BrokerPipe.cpp (Windows).

#include "Mocks/MockProbes.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokerRequestMapping.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <variant>
#include <vector>

namespace Platform
{
namespace
{

constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 99};

/// Actions whose every call throws, for handleRequest()'s no-throw promise.
class ThrowingActions : public TestMocks::MockProcessActions
{
  public:
    [[nodiscard]] ProcessActionResult terminate(const ProcessTarget& /*target*/) override
    {
        throw std::runtime_error("boom");
    }
};

[[nodiscard]] Broker::ActionResult actionResult(const Broker::Response& response)
{
    const auto* const result = std::get_if<Broker::ActionResult>(&response);
    return result != nullptr ? *result : Broker::ActionResult{.success = false, .errorMessage = "not an ActionResult", .successNote = {}};
}

TEST(BrokerRequestMappingTest, HelloOfThisVersionIsAccepted)
{
    TestMocks::MockProcessActions actions;
    const Broker::Response response = BrokerServer::handleRequest(Broker::Hello{}, actions);
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(response));
    EXPECT_TRUE(std::get<Broker::HelloReply>(response).accepted);
    EXPECT_EQ(std::get<Broker::HelloReply>(response).version, Broker::PROTOCOL_VERSION);
}

TEST(BrokerRequestMappingTest, HelloOfAnotherVersionIsRefused)
{
    TestMocks::MockProcessActions actions;
    const Broker::Response response = BrokerServer::handleRequest(Broker::Hello{.version = Broker::PROTOCOL_VERSION + 1}, actions);
    ASSERT_TRUE(std::holds_alternative<Broker::HelloReply>(response));
    EXPECT_FALSE(std::get<Broker::HelloReply>(response).accepted);
}

TEST(BrokerRequestMappingTest, EachSignalReachesItsAction)
{
    TestMocks::MockProcessActions actions;
    for (const Broker::Signal signal : {Broker::Signal::Terminate, Broker::Signal::Kill, Broker::Signal::Suspend, Broker::Signal::Resume})
    {
        EXPECT_TRUE(actionResult(BrokerServer::handleRequest(Broker::SignalRequest{.target = TARGET, .signal = signal}, actions)).success);
    }
    EXPECT_EQ(actions.terminateCount(), 1);
    EXPECT_EQ(actions.killCount(), 1);
    EXPECT_EQ(actions.stopCount(), 1);
    EXPECT_EQ(actions.resumeCount(), 1);
    EXPECT_EQ(actions.lastTarget(), TARGET);

    // The other signals (#3063) reach sendSignal() with their own ProcessSignal.
    for (const auto [wire, signal] : {std::pair{Broker::Signal::Hangup, ProcessSignal::Hangup},
                                      std::pair{Broker::Signal::Interrupt, ProcessSignal::Interrupt},
                                      std::pair{Broker::Signal::Quit, ProcessSignal::Quit},
                                      std::pair{Broker::Signal::User1, ProcessSignal::User1},
                                      std::pair{Broker::Signal::User2, ProcessSignal::User2}})
    {
        EXPECT_TRUE(actionResult(BrokerServer::handleRequest(Broker::SignalRequest{.target = TARGET, .signal = wire}, actions)).success);
        EXPECT_EQ(actions.lastSignal(), signal);
    }
    EXPECT_EQ(actions.sendSignalCount(), 5);
}

TEST(BrokerRequestMappingTest, PriorityIoPriorityAndAffinityReachTheirActions)
{
    TestMocks::MockProcessActions actions;
    EXPECT_TRUE(actionResult(BrokerServer::handleRequest(Broker::PriorityRequest{.target = TARGET, .nice = -5}, actions)).success);
    EXPECT_EQ(actions.setPriorityCount(), 1);
    EXPECT_EQ(actions.lastSetPriorityNice(), -5);

    EXPECT_TRUE(actionResult(BrokerServer::handleRequest(
                                 Broker::IoPriorityRequest{.target = TARGET, .ioClass = IoPriorityClass::Realtime, .level = 2}, actions))
                    .success);
    EXPECT_EQ(actions.setIoPriorityCount(), 1);
    EXPECT_EQ(actions.lastSetIoPriority().ioClass, IoPriorityClass::Realtime);
    EXPECT_EQ(actions.lastSetIoPriority().level, 2);

    EXPECT_TRUE(
        actionResult(BrokerServer::handleRequest(Broker::AffinityRequest{.target = TARGET, .cpus = CpuAffinity::fromMask(0b101)}, actions))
            .success);
    EXPECT_EQ(actions.setAffinityCount(), 1);
    EXPECT_EQ(actions.lastAffinity(), CpuAffinity::fromMask(0b101));
    EXPECT_EQ(actions.lastTarget(), TARGET);
}

TEST(BrokerRequestMappingTest, AFailedActionCarriesItsMessage)
{
    TestMocks::MockProcessActions actions;
    actions.setKillResult(ProcessActionResult::error("Permission denied"));
    const Broker::ActionResult result =
        actionResult(BrokerServer::handleRequest(Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill}, actions));
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "Permission denied");
}

TEST(BrokerRequestMappingTest, AnActionThatThrowsIsAFailedResult)
{
    ThrowingActions actions;
    const Broker::ActionResult result =
        actionResult(BrokerServer::handleRequest(Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Terminate}, actions));
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
}

TEST(BrokerRequestMappingTest, AnEnvironmentRequestIsReadThroughTheReader)
{
    TestMocks::MockProcessActions actions;
    TestMocks::MockProcessEnvironmentReader environment;
    environment.setResult({.status = EnvironmentReadStatus::Ok, .variables = {{.name = "HOME", .value = "/root"}}});
    const Broker::Response response =
        BrokerServer::handleRequest(Broker::EnvironmentRequest{.target = TARGET}, actions, {.environment = &environment});
    ASSERT_TRUE(std::holds_alternative<Broker::EnvironmentReply>(response));
    EXPECT_EQ(std::get<Broker::EnvironmentReply>(response).result.status, EnvironmentReadStatus::Ok);
    EXPECT_EQ(environment.lastTarget(), TARGET);
}

TEST(BrokerRequestMappingTest, AnEnvironmentRequestWithoutAReaderIsRefused)
{
    TestMocks::MockProcessActions actions;
    const Broker::Response response = BrokerServer::handleRequest(Broker::EnvironmentRequest{.target = TARGET}, actions);
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(response));
}

TEST(BrokerRequestMappingTest, ModulesAndOpenFilesRequestsAreReadThroughTheirReaders)
{
    TestMocks::MockProcessActions actions;
    TestMocks::MockProcessModulesReader modules;
    TestMocks::MockProcessOpenFilesReader openFiles;
    const BrokerServer::Readers readers{.environment = nullptr, .modules = &modules, .openFiles = &openFiles, .connections = nullptr};
    EXPECT_TRUE(std::holds_alternative<Broker::ModulesReply>(
        BrokerServer::handleRequest(Broker::ModulesRequest{.target = TARGET}, actions, readers)));
    EXPECT_EQ(modules.readCount(), 1);
    EXPECT_TRUE(std::holds_alternative<Broker::OpenFilesReply>(
        BrokerServer::handleRequest(Broker::OpenFilesRequest{.target = TARGET}, actions, readers)));
    EXPECT_EQ(openFiles.readCount(), 1);
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(BrokerServer::handleRequest(Broker::ModulesRequest{.target = TARGET}, actions)));
}

TEST(BrokerRequestMappingTest, AConnectionsRequestIsReadThroughItsReader)
{
    TestMocks::MockProcessActions actions;
    TestMocks::MockProcessConnectionsReader connections;
    const BrokerServer::Readers readers{.environment = nullptr, .modules = nullptr, .openFiles = nullptr, .connections = &connections};
    EXPECT_TRUE(std::holds_alternative<Broker::ConnectionsReply>(
        BrokerServer::handleRequest(Broker::ConnectionsRequest{.target = TARGET}, actions, readers)));
    EXPECT_EQ(connections.readCount(), 1);
    EXPECT_TRUE(
        std::holds_alternative<Broker::ErrorReply>(BrokerServer::handleRequest(Broker::ConnectionsRequest{.target = TARGET}, actions)));
}

TEST(BrokerRequestMappingTest, AFirmwareFileRequestIsReadThroughItsFunction)
{
    TestMocks::MockProcessActions actions;
    std::optional<Broker::FirmwareFile> asked;
    BrokerServer::Readers readers;
    readers.firmwareFile = [&asked](Broker::FirmwareFile file)
    {
        asked = file;
        return Broker::FirmwareFileReply{.status = Broker::FirmwareFileStatus::Ok, .bytes = {1, 2, 3}};
    };
    const Broker::Response response =
        BrokerServer::handleRequest(Broker::FirmwareFileRequest{.file = Broker::FirmwareFile::ProductUuid}, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::FirmwareFileReply>(response));
    EXPECT_EQ(std::get<Broker::FirmwareFileReply>(response).bytes, (std::vector<std::uint8_t>{1, 2, 3}));
    EXPECT_EQ(asked, Broker::FirmwareFile::ProductUuid);
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(
        BrokerServer::handleRequest(Broker::FirmwareFileRequest{.file = Broker::FirmwareFile::ProductUuid}, actions)));
}

TEST(BrokerRequestMappingTest, ACrashesRequestIsReadThroughItsFunction)
{
    TestMocks::MockProcessActions actions;
    BrokerServer::Readers readers;
    readers.crashes = []
    {
        CrashesInfo info;
        info.available = true;
        info.listed = true;
        return info;
    };
    const Broker::Response response = BrokerServer::handleRequest(Broker::CrashesRequest{}, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::CrashesReply>(response));
    EXPECT_TRUE(std::get<Broker::CrashesReply>(response).info.listed);
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(BrokerServer::handleRequest(Broker::CrashesRequest{}, actions)));
}

TEST(BrokerRequestMappingTest, AMemoryRollupRequestIsReadThroughItsFunction)
{
    TestMocks::MockProcessActions actions;
    ProcessTarget asked;
    BrokerServer::Readers readers;
    readers.memoryRollup = [&asked](std::int32_t pid, std::uint64_t start) -> MemoryRollupRead
    {
        asked = {.pid = pid, .startTimeTicks = start};
        return MemoryRollup{.pssBytes = 1, .ussBytes = 2, .swapPssBytes = 3};
    };
    const Broker::Response response = BrokerServer::handleRequest(Broker::MemoryRollupRequest{.target = TARGET}, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::MemoryRollupReply>(response));
    EXPECT_TRUE(std::get<Broker::MemoryRollupReply>(response).result.has_value());
    EXPECT_EQ(asked, TARGET);
    EXPECT_TRUE(
        std::holds_alternative<Broker::ErrorReply>(BrokerServer::handleRequest(Broker::MemoryRollupRequest{.target = TARGET}, actions)));
}

TEST(BrokerRequestMappingTest, AProcessCountersRequestReadsEachTargetAndLeavesOutTheGoneOrThrowing)
{
    TestMocks::MockProcessActions actions;
    const ProcessTarget gone{.pid = 8, .startTimeTicks = 1};
    const ProcessTarget throwing{.pid = 9, .startTimeTicks = 1};
    BrokerServer::Readers readers;
    bool askedForSockets = false;
    readers.processCounters = [&](const ProcessTarget& target, bool withSockets) -> std::optional<Broker::ProcessCountersEntry>
    {
        askedForSockets = askedForSockets || withSockets;
        if (target == gone)
        {
            return std::nullopt;
        }
        if (target == throwing)
        {
            throw std::runtime_error("boom");
        }
        return Broker::ProcessCountersEntry{.target = target, .handleCountRead = true, .handleCount = 4};
    };
    const Broker::Response response = BrokerServer::handleRequest(
        Broker::ProcessCountersRequest{.targets = {TARGET, gone, throwing}, .withSockets = true}, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::ProcessCountersReply>(response));
    const auto& entries = std::get<Broker::ProcessCountersReply>(response).entries;
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().target, TARGET);
    EXPECT_EQ(entries.front().handleCount, 4);
    EXPECT_TRUE(askedForSockets) << "the request's withSockets reaches the reader";
    // A helper without the reader (Windows', which reads every process itself) says so.
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(
        BrokerServer::handleRequest(Broker::ProcessCountersRequest{.targets = {TARGET}}, actions)));
}

TEST(BrokerRequestMappingTest, AHiddenProcessesRequestGoesToItsReaderWithTheVisiblePids)
{
    TestMocks::MockProcessActions actions;
    BrokerServer::Readers readers;
    std::vector<std::int32_t> seen;
    readers.hiddenProcesses = [&seen](std::span<const std::int32_t> visible)
    {
        seen.assign(visible.begin(), visible.end());
        return Broker::HiddenProcessesReply{.processes = {Broker::HiddenProcess{.target = TARGET, .stat = "x"}}, .truncated = false};
    };
    const Broker::Response response = BrokerServer::handleRequest(Broker::HiddenProcessesRequest{.visiblePids = {1, 2}}, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::HiddenProcessesReply>(response));
    EXPECT_EQ(std::get<Broker::HiddenProcessesReply>(response).processes.size(), 1U);
    EXPECT_EQ(seen, (std::vector<std::int32_t>{1, 2}));

    // One that throws lists none this time, marked truncated, so TaskSmack asks again (#3032); only a helper
    // without the reader (Windows', which has no hidepid) answers with the error that stops it asking.
    readers.hiddenProcesses = [](std::span<const std::int32_t>) -> Broker::HiddenProcessesReply
    {
        throw std::runtime_error("boom");
    };
    const Broker::Response thrown = BrokerServer::handleRequest(Broker::HiddenProcessesRequest{}, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::HiddenProcessesReply>(thrown));
    EXPECT_EQ(std::get<Broker::HiddenProcessesReply>(thrown), (Broker::HiddenProcessesReply{.processes = {}, .truncated = true}));
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(BrokerServer::handleRequest(Broker::HiddenProcessesRequest{}, actions)));
}

TEST(BrokerRequestMappingTest, AServiceActionGoesToItsReaderAndAThrowIsAFailedResult)
{
    TestMocks::MockProcessActions actions;
    BrokerServer::Readers readers;
    Broker::ServiceActionRequest seen;
    readers.serviceAction = [&seen](const Broker::ServiceActionRequest& request)
    {
        seen = request;
        return ServiceActionResult::succeeded();
    };
    const Broker::ServiceActionRequest stop{.action = ServiceAction::Stop, .unit = "cron.service", .startType = ServiceStartType::Unknown};
    const Broker::Response response = BrokerServer::handleRequest(stop, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::ServiceActionReply>(response));
    EXPECT_TRUE(std::get<Broker::ServiceActionReply>(response).result.ok);
    EXPECT_EQ(seen, stop);

    readers.serviceAction = [](const Broker::ServiceActionRequest&) -> ServiceActionResult
    {
        throw std::runtime_error("boom");
    };
    const Broker::Response threw = BrokerServer::handleRequest(stop, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::ServiceActionReply>(threw));
    EXPECT_FALSE(std::get<Broker::ServiceActionReply>(threw).result.ok);
    // A helper without the reader (Windows') answers with an error.
    EXPECT_TRUE(std::holds_alternative<Broker::ErrorReply>(BrokerServer::handleRequest(stop, actions)));
}

// --- #2417: what the helper does when a read or an action throws -------------------------------------

/// Readers whose every read throws, for handleRequest()'s no-throw promise.
class ThrowingEnvironmentReader final : public TestMocks::MockProcessEnvironmentReader
{
  public:
    [[nodiscard]] EnvironmentReadResult readEnvironment(const ProcessTarget& /*target*/) override
    {
        throw std::runtime_error("boom");
    }
};

class ThrowingModulesReader final : public TestMocks::MockProcessModulesReader
{
  public:
    [[nodiscard]] ModulesReadResult readModules(const ProcessTarget& /*target*/) override
    {
        throw std::runtime_error("boom");
    }
};

class ThrowingOpenFilesReader final : public TestMocks::MockProcessOpenFilesReader
{
  public:
    [[nodiscard]] OpenFilesReadResult readOpenFiles(const ProcessTarget& /*target*/) override
    {
        throw std::runtime_error("boom");
    }
};

class ThrowingConnectionsReader final : public TestMocks::MockProcessConnectionsReader
{
  public:
    [[nodiscard]] ConnectionsReadResult readConnections(const ProcessTarget& /*target*/) override
    {
        throw std::runtime_error("boom");
    }
};

/// Actions that throw something that isn't a std::exception.
class ThrowingNonStandardActions final : public TestMocks::MockProcessActions
{
  public:
    [[nodiscard]] ProcessActionResult kill(const ProcessTarget& /*target*/) override
    {
        throw 42; // not a std::exception: what the catch-all is for
    }
};

TEST(BrokerRequestMappingTest, AReadThatThrowsIsAFailedReadNotAnEndedHelper)
{
    TestMocks::MockProcessActions actions;
    ThrowingEnvironmentReader environment;
    ThrowingModulesReader modules;
    ThrowingOpenFilesReader openFiles;
    ThrowingConnectionsReader connections;
    BrokerServer::Readers readers{.environment = &environment, .modules = &modules, .openFiles = &openFiles, .connections = &connections};
    readers.memoryRollup = [](std::int32_t /*pid*/, std::uint64_t /*start*/) -> MemoryRollupRead
    {
        throw std::runtime_error("boom");
    };
    readers.firmwareFile = [](Broker::FirmwareFile /*file*/) -> Broker::FirmwareFileReply
    {
        throw std::runtime_error("boom");
    };
    readers.crashes = []() -> CrashesInfo
    {
        throw std::runtime_error("boom");
    };

    const Broker::Response env = BrokerServer::handleRequest(Broker::EnvironmentRequest{.target = TARGET}, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::EnvironmentReply>(env));
    EXPECT_EQ(std::get<Broker::EnvironmentReply>(env).result.status, EnvironmentReadStatus::Failed);

    const Broker::Response mods = BrokerServer::handleRequest(Broker::ModulesRequest{.target = TARGET}, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::ModulesReply>(mods));
    EXPECT_EQ(std::get<Broker::ModulesReply>(mods).result.status, ModulesReadStatus::Failed);

    const Broker::Response files = BrokerServer::handleRequest(Broker::OpenFilesRequest{.target = TARGET}, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::OpenFilesReply>(files));
    EXPECT_EQ(std::get<Broker::OpenFilesReply>(files).result.status, OpenFilesReadStatus::Failed);

    const Broker::Response sockets = BrokerServer::handleRequest(Broker::ConnectionsRequest{.target = TARGET}, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::ConnectionsReply>(sockets));
    EXPECT_EQ(std::get<Broker::ConnectionsReply>(sockets).result.status, ConnectionsReadStatus::Failed);

    const Broker::Response rollup = BrokerServer::handleRequest(Broker::MemoryRollupRequest{.target = TARGET}, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::MemoryRollupReply>(rollup));
    const MemoryRollupRead& read = std::get<Broker::MemoryRollupReply>(rollup).result;
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error(), MemoryRollupFailure::Unavailable);

    const Broker::Response firmware =
        BrokerServer::handleRequest(Broker::FirmwareFileRequest{.file = Broker::FirmwareFile::SmbiosTable}, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::FirmwareFileReply>(firmware));
    EXPECT_EQ(std::get<Broker::FirmwareFileReply>(firmware).status, Broker::FirmwareFileStatus::Unreadable);

    const Broker::Response crashes = BrokerServer::handleRequest(Broker::CrashesRequest{}, actions, readers);
    ASSERT_TRUE(std::holds_alternative<Broker::ErrorReply>(crashes));
    EXPECT_EQ(std::get<Broker::ErrorReply>(crashes).message, "The crash read failed");
}

TEST(BrokerRequestMappingTest, AnOpenFilesRequestWithoutAReaderIsRefused)
{
    TestMocks::MockProcessActions actions;
    const Broker::Response response = BrokerServer::handleRequest(Broker::OpenFilesRequest{.target = TARGET}, actions);
    ASSERT_TRUE(std::holds_alternative<Broker::ErrorReply>(response));
    EXPECT_EQ(std::get<Broker::ErrorReply>(response).message, "This helper doesn't list open files");
}

TEST(BrokerRequestMappingTest, AnActionThatThrowsANonStandardExceptionIsAFailedResult)
{
    ThrowingNonStandardActions actions;
    const Broker::ActionResult result =
        actionResult(BrokerServer::handleRequest(Broker::SignalRequest{.target = TARGET, .signal = Broker::Signal::Kill}, actions));
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "The action failed");
}

} // namespace
} // namespace Platform
