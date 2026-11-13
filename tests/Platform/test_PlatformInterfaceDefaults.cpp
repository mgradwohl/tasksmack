/// @file test_PlatformInterfaceDefaults.cpp
/// @brief The Platform interfaces' default methods (#3374): an implementation that overrides only the required
/// ones gets each optional method's documented default, and the Unsupported implementations refuse.

#include "Platform/IGPUProbe.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessMemoryMap.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IProcessThreads.h"
#include "Platform/IServiceActions.h"
#include "Platform/IStartAtLogin.h"
#include "Platform/IStartupActions.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform
{
namespace
{

/// Only IProcessActions' pure virtual methods: everything else is the interface's default.
class RequiredOnlyActions : public IProcessActions
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
        return IoPriority{};
    }
};

constexpr ProcessTarget TARGET{.pid = 4242, .startTimeTicks = 99};

TEST(PlatformInterfaceDefaultsTest, TheOptionalActionsAreRefusedWithTheirOwnReason)
{
    RequiredOnlyActions actions;

    const ProcessActionResult signal = actions.sendSignal(TARGET, ProcessSignal::Hangup);
    EXPECT_FALSE(signal.success);
    EXPECT_EQ(signal.errorMessage, "Sending this signal isn't available on this system");
    EXPECT_FALSE(signal.permissionDenied);

    const ProcessActionResult efficiency = actions.setEfficiencyMode(TARGET, true);
    EXPECT_FALSE(efficiency.success);
    EXPECT_EQ(efficiency.errorMessage, "Efficiency mode isn't available on this system");
    EXPECT_FALSE(efficiency.permissionDenied);

    const ProcessActionResult close = actions.closeConnection(TARGET, 12345);
    EXPECT_FALSE(close.success);
    EXPECT_EQ(close.errorMessage, "Closing a connection isn't available on this system");
    EXPECT_FALSE(close.permissionDenied);

    const ProcessActionResult trace = actions.launchSyscallTrace(TARGET);
    EXPECT_FALSE(trace.success);
    EXPECT_EQ(trace.errorMessage, syscallTraceUnavailableReason(SyscallTraceAvailability::Unsupported));
}

TEST(PlatformInterfaceDefaultsTest, EfficiencyModeStateIsNeverRemembered)
{
    RequiredOnlyActions actions;
    EXPECT_FALSE(actions.efficiencyNiceToRestore(TARGET).has_value());
    EXPECT_FALSE(actions.efficiencyPriorityClassToRestore(TARGET).has_value());

    // NORMAL_PRIORITY_CLASS (0x20): a real class, which an implementation that remembers would keep.
    constexpr std::uint32_t NORMAL_CLASS = 0x20;
    actions.rememberEfficiencyPriorityClass(TARGET, NORMAL_CLASS);
    EXPECT_FALSE(actions.efficiencyPriorityClassToRestore(TARGET).has_value());

    actions.setEfficiencyPriorityClassToRestore(TARGET, NORMAL_CLASS);
    EXPECT_FALSE(actions.efficiencyPriorityClassToRestore(TARGET).has_value());

    actions.forgetEfficiencyPriorityClass(TARGET);
    EXPECT_FALSE(actions.efficiencyPriorityClassToRestore(TARGET).has_value());
}

TEST(PlatformInterfaceDefaultsTest, OnlyAnAvailableTraceHasNoReason)
{
    EXPECT_STREQ(syscallTraceUnavailableReason(SyscallTraceAvailability::Available), "");
    for (const SyscallTraceAvailability missing :
         {SyscallTraceAvailability::Unsupported, SyscallTraceAvailability::NoTracer, SyscallTraceAvailability::NoTerminal})
    {
        EXPECT_NE(std::string(syscallTraceUnavailableReason(missing)), "") << static_cast<int>(missing);
    }
}

TEST(PlatformInterfaceDefaultsTest, AnImplementationCanBeCopiedAndMoved)
{
    RequiredOnlyActions original;
    RequiredOnlyActions copy(original);
    RequiredOnlyActions moved(std::move(copy));
    RequiredOnlyActions assigned;
    assigned = original;
    assigned = std::move(moved);
    EXPECT_TRUE(assigned.terminate(TARGET).success);
    EXPECT_FALSE(assigned.sendSignal(TARGET, ProcessSignal::Hangup).success);
}

/// Only the required readers: hasMemoryMap() and readMemoryMap(), which returns a status to recognise.
class RequiredOnlyMemoryMapReader : public IProcessMemoryMapReader
{
  public:
    [[nodiscard]] bool hasMemoryMap() const override
    {
        return true;
    }
    [[nodiscard]] MemoryMapReadResult readMemoryMap(const ProcessTarget& /*target*/) override
    {
        ++reads;
        return {.status = MemoryMapReadStatus::PermissionDenied, .mappings = {}, .detail = "plain read"};
    }
    int reads = 0;
};

TEST(PlatformInterfaceDefaultsTest, AMemoryMapReaderWithoutAHelperReadsAsThePlainReadDoes)
{
    RequiredOnlyMemoryMapReader reader;
    EXPECT_FALSE(reader.canReadAuthorized());
    const MemoryMapReadResult result = reader.readAuthorized(TARGET, std::stop_token{});
    EXPECT_EQ(reader.reads, 1);
    EXPECT_EQ(result.status, MemoryMapReadStatus::PermissionDenied);
    EXPECT_EQ(result.detail, "plain read");

    UnsupportedProcessMemoryMapReader unsupported;
    EXPECT_FALSE(unsupported.hasMemoryMap());
    EXPECT_EQ(unsupported.readMemoryMap(TARGET).status, MemoryMapReadStatus::Unsupported);
}

class RequiredOnlyThreadsReader : public IProcessThreadsReader
{
  public:
    [[nodiscard]] bool hasThreads() const override
    {
        return true;
    }
    [[nodiscard]] ThreadsReadResult readThreads(const ProcessTarget& /*target*/) override
    {
        ++reads;
        return {.status = ThreadsReadStatus::PermissionDenied, .threads = {}, .detail = "plain read"};
    }
    int reads = 0;
};

TEST(PlatformInterfaceDefaultsTest, AThreadsReaderWithoutAHelperReadsAsThePlainReadDoes)
{
    RequiredOnlyThreadsReader reader;
    EXPECT_FALSE(reader.canReadAuthorized());
    const ThreadsReadResult result = reader.readAuthorized(TARGET, std::stop_token{});
    EXPECT_EQ(reader.reads, 1);
    EXPECT_EQ(result.status, ThreadsReadStatus::PermissionDenied);
    EXPECT_EQ(result.detail, "plain read");

    UnsupportedProcessThreadsReader unsupported;
    EXPECT_FALSE(unsupported.hasThreads());
    EXPECT_EQ(unsupported.readThreads(TARGET).status, ThreadsReadStatus::Unsupported);
}

/// Each action answers with its own name, so runAuthorized()'s default can be seen to pick the plain call.
class NamingServiceActions : public IServiceActions
{
  public:
    [[nodiscard]] ServiceActionCapabilities capabilities() const override
    {
        return {};
    }
    [[nodiscard]] ServiceActionResult start(std::string_view name, const std::stop_token& /*stopToken*/) override
    {
        return ServiceActionResult::failed("start " + std::string(name));
    }
    [[nodiscard]] ServiceActionResult stop(std::string_view name, const std::stop_token& /*stopToken*/) override
    {
        return ServiceActionResult::failed("stop " + std::string(name));
    }
    [[nodiscard]] ServiceActionResult restart(std::string_view name, const std::stop_token& /*stopToken*/) override
    {
        return ServiceActionResult::failed("restart " + std::string(name));
    }
    [[nodiscard]] ServiceActionResult setStartType(std::string_view name, ServiceStartType startType) override
    {
        lastStartType = startType;
        return ServiceActionResult::failed("setStartType " + std::string(name));
    }
    ServiceStartType lastStartType{};
};

TEST(PlatformInterfaceDefaultsTest, RunAuthorizedWithoutAPromptIsThePlainCall)
{
    NamingServiceActions actions;
    const std::stop_token none;
    EXPECT_EQ(actions.runAuthorized(ServiceAction::Start, "a.service", {}, none).message, "start a.service");
    EXPECT_EQ(actions.runAuthorized(ServiceAction::Stop, "a.service", {}, none).message, "stop a.service");
    EXPECT_EQ(actions.runAuthorized(ServiceAction::Restart, "a.service", {}, none).message, "restart a.service");
    EXPECT_EQ(actions.runAuthorized(ServiceAction::SetStartType, "a.service", ServiceStartType::Disabled, none).message,
              "setStartType a.service");
    EXPECT_EQ(actions.lastStartType, ServiceStartType::Disabled);

    // A value outside the enum, as a corrupt request could carry, is refused rather than run.
    const auto unknown = static_cast<ServiceAction>(0xFF);
    const ServiceActionResult refused = actions.runAuthorized(unknown, "a.service", {}, none);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.message, "Unknown action");
}

TEST(PlatformInterfaceDefaultsTest, OnlyAnEntryWithAStatePolicyDoesntSetAndNoReasonIsSwitchable)
{
    StartupEntry entry;
    entry.location = StartupLocation::RunUser;
    EXPECT_TRUE(hasSwitchableState(entry));

    entry.location = StartupLocation::RunOnceUser;
    EXPECT_FALSE(hasSwitchableState(entry));
    entry.location = StartupLocation::RunOnceMachine;
    EXPECT_FALSE(hasSwitchableState(entry));

    entry.location = StartupLocation::RunUser;
    entry.setByPolicy = true;
    EXPECT_FALSE(hasSwitchableState(entry));

    entry.setByPolicy = false;
    entry.notSwitchableReason = "Managed by the desktop";
    EXPECT_FALSE(hasSwitchableState(entry));

    UnsupportedStartupActions unsupported;
    const StartupActionResult result = unsupported.setEnabled(StartupEntry{}, true);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.message, "Startup apps can't be changed on this platform yet");
}

/// call() records the stop token it was given and answers every request.
class RequiredOnlyBroker : public IPrivilegedBroker
{
  public:
    [[nodiscard]] bool connected() const noexcept override
    {
        return true;
    }
    [[nodiscard]] bool lastSeenConnected() const noexcept override
    {
        return true;
    }
    [[nodiscard]] std::expected<void, std::string> connect(const std::stop_token& /*stopToken*/) override
    {
        return {};
    }
    [[nodiscard]] std::expected<Broker::Response, std::string> call(const Broker::Request& /*request*/,
                                                                    const std::stop_token& stopToken) override
    {
        ++calls;
        stopPossible = stopToken.stop_possible();
        return Broker::Response{};
    }
    int calls = 0;
    bool stopPossible = true;
};

TEST(PlatformInterfaceDefaultsTest, ABrokerWithoutContentionCallsWhenAskedToCallIfIdle)
{
    RequiredOnlyBroker broker;
    EXPECT_TRUE(broker.readsOtherUsers());
    EXPECT_EQ(broker.sessionsStarted(), 0U);

    EXPECT_TRUE(broker.callIfIdle(Broker::Request{}, std::chrono::milliseconds(50)).has_value());
    EXPECT_EQ(broker.calls, 1);
    EXPECT_FALSE(broker.stopPossible); // a default std::stop_token: the timeout is call()'s own bounded wait
}

class RequiredOnlyGpuProbe : public IGPUProbe
{
  public:
    [[nodiscard]] std::vector<GPUInfo> enumerateGPUs() override
    {
        return {};
    }
    [[nodiscard]] std::vector<GPUCounters> readGPUCounters() override
    {
        return {};
    }
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters() override
    {
        return {};
    }
    [[nodiscard]] GPUCapabilities capabilities() const override
    {
        return {};
    }
};

TEST(PlatformInterfaceDefaultsTest, AGpuProbeWithAFixedDeviceSetReportsNoChangeOnARescan)
{
    RequiredOnlyGpuProbe probe;
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));
}

class RequiredOnlyStartAtLogin : public IStartAtLogin
{
  public:
    [[nodiscard]] std::expected<StartAtLoginFacts, std::string> read() override
    {
        return StartAtLoginFacts{};
    }
    [[nodiscard]] std::expected<void, std::string> write(std::string_view /*command*/) override
    {
        return {};
    }
    [[nodiscard]] std::expected<void, std::string> remove() override
    {
        return {};
    }
};

TEST(PlatformInterfaceDefaultsTest, StartAtLoginWithoutAnUninstallerRestoresNothingAndHasNoMark)
{
    RequiredOnlyStartAtLogin startAtLogin;
    EXPECT_FALSE(startAtLogin.restoresMissingEntry());
    const std::expected<bool, std::string> mark = startAtLogin.takeUninstallMark();
    ASSERT_TRUE(mark.has_value());
    EXPECT_FALSE(*mark);
}

class RequiredOnlySystemInfoProbe : public ISystemInfoProbe
{
  public:
    [[nodiscard]] SystemInfoCapabilities capabilities() const override
    {
        return {};
    }
    [[nodiscard]] OsInfo readOs() override
    {
        return {};
    }
    [[nodiscard]] FirmwareInfo readFirmware() override
    {
        return {};
    }
    [[nodiscard]] MemoryModulesInfo readMemoryModules() override
    {
        return {};
    }
    [[nodiscard]] CommitPagingInfo readCommitPaging() override
    {
        return {};
    }
    [[nodiscard]] StorageInfo readStorage() override
    {
        return {};
    }
    [[nodiscard]] GraphicsInfo readGraphics() override
    {
        return {};
    }
    [[nodiscard]] PlatformSecurityInfo readPlatformSecurity() override
    {
        return {};
    }
    [[nodiscard]] SensorsInfo readSensors() override
    {
        return {};
    }
    [[nodiscard]] DevicesInfo readDevices() override
    {
        return {};
    }
    [[nodiscard]] DriversInfo readDrivers() override
    {
        return {};
    }
    [[nodiscard]] CrashesInfo readCrashes() override
    {
        return {};
    }
    [[nodiscard]] NetworkAdaptersInfo readNetworkAdapters(bool /*includeWifiDetails*/) override
    {
        return {};
    }
    [[nodiscard]] BootPerformanceInfo readBootPerformance() override
    {
        return {};
    }
};

TEST(PlatformInterfaceDefaultsTest, ASystemInfoProbeWithoutALookupOrHelperReadsNoPublicIpAndCantAuthorize)
{
    RequiredOnlySystemInfoProbe probe;
    const PublicIpInfo ip = probe.readPublicIp();
    EXPECT_FALSE(ip.read);
    EXPECT_TRUE(ip.v4.empty());
    EXPECT_TRUE(ip.v6.empty());
    EXPECT_FALSE(probe.canReadAuthorized());
    EXPECT_FALSE(probe.authorize(std::stop_token{}));
}

class RequiredOnlyProcessProbe : public IProcessProbe
{
  public:
    [[nodiscard]] std::vector<ProcessCounters> enumerate() override
    {
        return {};
    }
    [[nodiscard]] ProcessCapabilities capabilities() const override
    {
        return {};
    }
    [[nodiscard]] std::uint64_t totalCpuTime() const override
    {
        return 0;
    }
    [[nodiscard]] long ticksPerSecond() const override
    {
        return 100;
    }
    [[nodiscard]] std::uint64_t systemTotalMemory() const override
    {
        return 0;
    }
};

TEST(PlatformInterfaceDefaultsTest, AProcessProbeWithoutRollupsOrUdpReadsNoneAndIgnoresTheCollector)
{
    RequiredOnlyProcessProbe probe;
    const MemoryRollupRead rollup = probe.readMemoryRollup(1234, 99);
    ASSERT_FALSE(rollup.has_value());
    EXPECT_EQ(rollup.error(), MemoryRollupFailure::Unavailable);

    probe.setUdpTrafficCollector(nullptr);
    probe.setSocketStatsCacheTtl(std::chrono::milliseconds(500));
    EXPECT_FALSE(probe.capabilities().hasUdpNetworkCounters);
}

} // namespace
} // namespace Platform
