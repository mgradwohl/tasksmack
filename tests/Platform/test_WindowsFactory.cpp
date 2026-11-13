// The Windows Platform factory (#3676): every product it makes is the Windows implementation, never an Unsupported
// stand-in or the wrong reader (#3089), and says what Windows can do. Making them reads nothing the tests depend on,
// raises no UAC prompt and starts no helper: nothing here connects, acts or calls showRunningTaskSmack(), which
// would bring another TaskSmack's window forward, or enableDebugPrivilege(), which would change this test process's
// own token for the tests after it.

#include "Platform/Factory.h"
// NOLINTBEGIN(misc-include-cleaner) - complete types for the returned pointers to destroy
#include "Platform/IGPUProbe.h"
#include "Platform/IHttpsClient.h"
#include "Platform/IPathProvider.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IServiceProbe.h"
#include "Platform/IStartAtLogin.h"
#include "Platform/ITaskManagerReplacement.h"
#include "Platform/IUdpTrafficCollector.h"
// NOLINTEND(misc-include-cleaner)
#include "Platform/IDiskProbe.h"
#include "Platform/IElevatedRestart.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessIconSource.h"
#include "Platform/IProcessMemoryMap.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IProcessSecurity.h"
#include "Platform/IProcessThreads.h"
#include "Platform/IServiceActions.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/ISystemConnections.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/IUserSessions.h"
#include "Platform/SystemTypes.h"
#include "Platform/TaskManagerReplacementService.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/ElevatedRestart.h"
#include "Platform/Windows/HelperLauncher.h"
#include "Platform/Windows/WindowsDiskProbe.h"
#include "Platform/Windows/WindowsElevation.h"
#include "Platform/Windows/WindowsEtwNetCollector.h"
#include "Platform/Windows/WindowsGPUProbe.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsHttpsClient.h"
#include "Platform/Windows/WindowsPathProvider.h"
#include "Platform/Windows/WindowsPowerProbe.h"
#include "Platform/Windows/WindowsProcessActions.h"
#include "Platform/Windows/WindowsProcessConnections.h"
#include "Platform/Windows/WindowsProcessEnvironmentReader.h"
#include "Platform/Windows/WindowsProcessIconSource.h"
#include "Platform/Windows/WindowsProcessMemoryMap.h"
#include "Platform/Windows/WindowsProcessModules.h"
#include "Platform/Windows/WindowsProcessOpenFiles.h"
#include "Platform/Windows/WindowsProcessProbe.h"
#include "Platform/Windows/WindowsProcessSecurity.h"
#include "Platform/Windows/WindowsProcessThreads.h"
#include "Platform/Windows/WindowsServiceActions.h"
#include "Platform/Windows/WindowsServiceProbe.h"
#include "Platform/Windows/WindowsStartAtLogin.h"
#include "Platform/Windows/WindowsStartupActions.h"
#include "Platform/Windows/WindowsStartupProbe.h"
#include "Platform/Windows/WindowsSystemConnectionsReader.h"
#include "Platform/Windows/WindowsSystemInfoProbe.h"
#include "Platform/Windows/WindowsSystemProbe.h"
#include "Platform/Windows/WindowsUserSessions.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

namespace Platform
{
namespace
{

/// Whether @p made is a @p Concrete: the implementation the factory is meant to make.
template<typename Concrete, typename Pointer> [[nodiscard]] bool is(const Pointer& made)
{
    return dynamic_cast<const Concrete*>(made.get()) != nullptr;
}

/// This process's token's elevation type: what the Restart as administrator prompt asks for.
[[nodiscard]] TOKEN_ELEVATION_TYPE ownElevationType()
{
    Windows::UniqueHandle token;
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD size = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE ||
        GetTokenInformation(token.get(), TokenElevationType, &type, sizeof(type), &size) == FALSE)
    {
        ADD_FAILURE() << "couldn't read this process's elevation type: " << GetLastError();
    }
    return type;
}

TEST(WindowsFactoryTest, TheProbesAreWindowsOnesThatSayWhatWindowsHas)
{
    const auto process = makeProcessProbe();
    ASSERT_TRUE(is<WindowsProcessProbe>(process));
    EXPECT_TRUE(process->capabilities().hasIoCounters);

    const auto system = makeSystemProbe();
    ASSERT_TRUE(is<WindowsSystemProbe>(system));
    const SystemCapabilities systemCaps = system->capabilities();
    EXPECT_TRUE(systemCaps.hasPerCoreCpu);
    EXPECT_TRUE(systemCaps.hasCommitCharge);
    EXPECT_FALSE(systemCaps.hasLoadAvg) << "Windows has no load average";
    EXPECT_FALSE(systemCaps.hasIoWait);

    const auto disk = makeDiskProbe();
    ASSERT_TRUE(is<WindowsDiskProbe>(disk));
    EXPECT_TRUE(disk->capabilities().hasDiskStats);
    EXPECT_TRUE(disk->capabilities().canFilterPhysical);

    EXPECT_TRUE(is<WindowsPowerProbe>(makePowerProbe()));
    EXPECT_TRUE(is<WindowsGPUProbe>(makeGPUProbe()));
    EXPECT_TRUE(is<WindowsPathProvider>(makePathProvider()));

    const auto startup = makeStartupProbe();
    ASSERT_TRUE(is<WindowsStartupProbe>(startup));
    EXPECT_TRUE(startup->capabilities().canEnumerate);
    EXPECT_TRUE(startup->capabilities().hasImpact);

    EXPECT_TRUE(is<WindowsServiceProbe>(makeServiceProbe()));

    const auto info = makeSystemInfoProbe();
    ASSERT_TRUE(is<WindowsSystemInfoProbe>(info));
    EXPECT_TRUE(info->capabilities().hasOs);

    EXPECT_TRUE(is<WindowsEtwNetCollector>(makeUdpTrafficCollector())) << "made, not started";
}

// #3089: a reader the factory got wrong (another's, or an Unsupported one) shows as a tab that is missing or empty.
TEST(WindowsFactoryTest, EachProcessReaderIsTheWindowsOneAndReads)
{
    const auto environment = makeProcessEnvironmentReader();
    ASSERT_TRUE(is<Windows::WindowsProcessEnvironmentReader>(environment));
    EXPECT_TRUE(environment->hasEnvironment());

    const auto connections = makeProcessConnectionsReader();
    ASSERT_TRUE(is<Windows::WindowsProcessConnectionsReader>(connections));
    EXPECT_TRUE(connections->hasConnections());

    const auto modules = makeProcessModulesReader();
    ASSERT_TRUE(is<Windows::WindowsProcessModulesReader>(modules));
    EXPECT_TRUE(modules->hasModules());

    const auto threads = makeProcessThreadsReader();
    ASSERT_TRUE(is<Windows::WindowsProcessThreadsReader>(threads));
    EXPECT_TRUE(threads->hasThreads());

    const auto memoryMap = makeProcessMemoryMapReader();
    ASSERT_TRUE(is<Windows::WindowsProcessMemoryMapReader>(memoryMap));
    EXPECT_TRUE(memoryMap->hasMemoryMap());

    const auto openFiles = makeProcessOpenFilesReader();
    ASSERT_TRUE(is<Windows::WindowsProcessOpenFilesReader>(openFiles));
    EXPECT_TRUE(openFiles->hasOpenFiles());

    const auto security = makeProcessSecurityReader();
    ASSERT_TRUE(is<Windows::WindowsProcessSecurityReader>(security));
    EXPECT_TRUE(security->hasSecurity());

    const auto icons = makeProcessIconSource();
    ASSERT_TRUE(is<WindowsProcessIconSource>(icons));
    EXPECT_TRUE(icons->caseInsensitivePaths()) << "NTFS paths";
}

TEST(WindowsFactoryTest, TheSystemWideReadersAreTheWindowsOnes)
{
    const auto connections = makeSystemConnectionsReader();
    ASSERT_TRUE(is<Windows::WindowsSystemConnectionsReader>(connections));
    EXPECT_TRUE(connections->hasConnections());

    const auto sessions = makeUserSessions();
    ASSERT_TRUE(is<Windows::WindowsUserSessions>(sessions));
    EXPECT_TRUE(sessions->hasSessions());

    EXPECT_TRUE(is<WindowsHttpsClient>(makeHttpsClient()));
    EXPECT_TRUE(is<WindowsStartAtLogin>(makeStartAtLogin()));
}

// The actions say whether TaskSmack runs elevated as the token does: the UI warns, or doesn't, from that.
TEST(WindowsFactoryTest, TheActionsAreTheWindowsOnesAndKnowWhetherTaskSmackIsElevated)
{
    const bool elevated = Windows::isProcessElevated();

    const auto processActions = makeProcessActions();
    ASSERT_TRUE(is<WindowsProcessActions>(processActions));
    const ProcessActionCapabilities processCaps = processActions->actionCapabilities();
    EXPECT_TRUE(processCaps.canKill);
    EXPECT_TRUE(processCaps.canSetPriority);
    EXPECT_FALSE(processCaps.canSetRealtimeIoPriority) << "Linux's class";

    const auto startupActions = makeStartupActions();
    ASSERT_TRUE(is<WindowsStartupActions>(startupActions));
    EXPECT_TRUE(startupActions->capabilities().canSetEnabled);
    EXPECT_EQ(startupActions->capabilities().elevated, elevated);

    const auto serviceActions = makeServiceActions();
    ASSERT_TRUE(is<WindowsServiceActions>(serviceActions));
    const ServiceActionCapabilities serviceCaps = serviceActions->capabilities();
    EXPECT_TRUE(serviceCaps.canStart && serviceCaps.canStop && serviceCaps.canRestart && serviceCaps.canSetStartType);
    EXPECT_EQ(serviceCaps.elevated, elevated);
}

// The administrator helper and Restart as administrator are offered only where they can work (#2550, #2728).
TEST(WindowsFactoryTest, TheElevationProductsAreOfferedOnlyWhereTheyCanWork)
{
    const bool elevated = Windows::isProcessElevated();

    const auto restart = makeElevatedRestart(nullptr);
    if (elevated)
    {
        EXPECT_EQ(restart, nullptr) << "already Administrator: nothing to restart into";
    }
    else
    {
        ASSERT_TRUE(is<Windows::ElevatedRestart::WindowsElevatedRestart>(restart));
        EXPECT_EQ(restart->prompt(), Windows::ElevatedRestart::promptFor(ownElevationType()));
    }

    // Made, never connected: no prompt. Offered exactly when availability() says the helper can serve this copy.
    const auto broker = makePrivilegedBroker(nullptr);
    const auto available = Windows::HelperLauncher::availability();
    EXPECT_EQ(broker != nullptr, available.has_value()) << available.error_or("available");
    if (elevated)
    {
        EXPECT_EQ(broker, nullptr) << "an elevated TaskSmack needs no helper";
    }
    if (broker != nullptr)
    {
        EXPECT_TRUE(is<Windows::HelperLauncher::LearnedReadsBroker>(broker));
        EXPECT_FALSE(broker->connected());
        EXPECT_EQ(broker->readsOtherUsers(), Windows::HelperLauncher::readsOtherUsers());
    }

    // Replace Task Manager, made with no broker: the service, whichever way this copy can or can't change the key.
    EXPECT_TRUE(is<TaskManagerReplacementService>(makeTaskManagerReplacement(nullptr)));
}

TEST(WindowsFactoryTest, TheHandOffWaitAndFolderCheckAreTheWindowsOnes)
{
    // This process, named with another start time: not the one that restarted anything, so not waited for.
    const std::uint64_t ticks = Windows::BrokerHelper::startTicksOf(GetCurrentProcess()).value_or(0);
    ASSERT_NE(ticks, 0U);
    EXPECT_EQ(waitForHandoff(GetCurrentProcessId(), ticks + 1), HandoffWait::NotThatProcess);

    // A folder outside every user's profile is refused before anything is read or held.
    const auto checked = checkHandedOverConfigDir(std::filesystem::path(L"TaskSmack"));
    ASSERT_FALSE(checked.has_value());
    EXPECT_TRUE(checked.error().contains("not a plain local folder path")) << checked.error();
}

} // namespace
} // namespace Platform
