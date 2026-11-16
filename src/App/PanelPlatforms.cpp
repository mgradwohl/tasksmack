#include "PanelPlatforms.h"

#include "App/PanelPlatformTypes.h"
#include "NativeWindow.h"
#ifdef _WIN32
// The helper's decorators, Windows only (#3076, #3784): Linux has no helper (#3580)
#include "Platform/BrokeredProcessActions.h"
#include "Platform/BrokeredUserSessions.h"
#endif
#include "Platform/Factory.h"
#include "Platform/IDiskProbe.h"
#include "Platform/IElevatedRestart.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IHttpsClient.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessMemoryMap.h"
#include "Platform/IStartAtLogin.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/IUserSessions.h"
#include "SessionAccountCheck.h"
#include "SyntheticScenario.h"
#include "TaskManagerReplacementController.h"

#include <memory>
#include <utility>

namespace App
{

namespace
{

/// The broker a test put in place of the installed helper's (Detail::setSharedPrivilegedBrokerForTesting()).
[[nodiscard]] std::shared_ptr<Platform::IPrivilegedBroker>& brokerForTesting()
{
    static std::shared_ptr<Platform::IPrivilegedBroker> broker;
    return broker;
}

/// The one privileged broker every real run's IProcessActions shares (#2308): made once, on first use,
/// and started only when the user first chooses "Retry as administrator" or "Read as administrator" (Windows).
/// Null where there is none: on Linux always (#3580), and on Windows in a copy without the helper. Its prompt belongs to the main window
/// (#2550), which exists before any panel asks for this. A test's broker, while one is set, comes first.
[[nodiscard]] const std::shared_ptr<Platform::IPrivilegedBroker>& sharedPrivilegedBroker()
{
    if (const std::shared_ptr<Platform::IPrivilegedBroker>& replaced = brokerForTesting(); replaced != nullptr)
    {
        return replaced;
    }
    static const std::shared_ptr<Platform::IPrivilegedBroker> broker = Platform::makePrivilegedBroker(mainNativeWindow());
    return broker;
}

/// What @p make makes for @p scenario: a synthetic run's as it is, and a real run's behind the shared broker, so
/// that an administrator can finish what TaskSmack's own call is refused (#2308, #3814). Every reader or action set
/// the helper can stand in for is made through this, so none can miss the broker on one path.
template<typename Make> [[nodiscard]] auto brokered(const Synthetic::Scenario* scenario, Make make) -> decltype(make(scenario))
{
    decltype(make(scenario)) made = make(scenario);
#ifdef _WIN32
    if (scenario != nullptr)
    {
        return made; // a synthetic run uses no helper
    }
    return Platform::withPrivilegedBroker(std::move(made), sharedPrivilegedBroker());
#else
    static_cast<void>(scenario);
    return made; // Linux has no helper (#3580), so nothing to decorate (#3784)
#endif
}

/// @p scenario's process actions, or the platform's, which an administrator can finish (#2308).
[[nodiscard]] std::unique_ptr<Platform::IProcessActions> processActionsFor(const Synthetic::Scenario* scenario)
{
    return brokered(scenario, Synthetic::makeProcessActions);
}

} // namespace

void preparePanelPlatforms(const Synthetic::Scenario* scenario)
{
    if (scenario != nullptr)
    {
        return; // a synthetic run uses no helper
    }
    // Made now, on the UI thread: its owner window handle is an SDL query (mainNativeWindow()), and the
    // factories below, which share it, run on the panels' sampler threads from here on (#2731).
    (void) sharedPrivilegedBroker();
}

std::shared_ptr<Platform::IElevatedRestart> makeElevatedRestart(const Synthetic::Scenario* scenario)
{
    if (scenario != nullptr)
    {
        return nullptr; // a synthetic run restarts nothing real
    }
    return Platform::makeElevatedRestart(mainNativeWindow());
}

std::shared_ptr<TaskManagerReplacementController> makeTaskManagerReplacementController(const Synthetic::Scenario* scenario)
{
    if (scenario != nullptr)
    {
        return std::make_shared<TaskManagerReplacementController>(nullptr); // a synthetic run changes nothing real
    }
    return std::make_shared<TaskManagerReplacementController>(Platform::makeTaskManagerReplacement(sharedPrivilegedBroker()));
}

ProcessesPanelPlatform makeProcessesPanelPlatform(const Synthetic::Scenario* scenario)
{
    // The probe reads every process itself: no helper read joins it since protocol v23 (#3580).
    ProcessesPanelPlatform platform{
        .probe = Synthetic::makeProcessProbe(scenario),
        .actions = processActionsFor(scenario),
        // Windows' ETW collector, or on Linux the eBPF counter TaskSmack loads when started as root (#3366, #3580).
        .udpCollector = Synthetic::makeUdpTrafficCollector(scenario),
        .icons = Synthetic::makeProcessIconSource(scenario),
    };
    // Find handles (#3067): the readers Process Details uses, another user's processes through the helper.
    // A search reads the open files in one batch (#3357): on Windows, one pass over the system handle table.
    platform.openFiles = brokered(scenario, Synthetic::makeProcessOpenFilesReader);
    platform.modules = brokered(scenario, Synthetic::makeProcessModulesReader);
    return platform;
}

SystemModelProbes makeSystemModelProbes(const Synthetic::Scenario* scenario)
{
    return {.system = Synthetic::makeSystemProbe(scenario), .power = Synthetic::makePowerProbe(scenario)};
}

std::unique_ptr<Platform::IDiskProbe> makeStorageModelProbe(const Synthetic::Scenario* scenario)
{
    return Synthetic::makeDiskProbe(scenario);
}

std::unique_ptr<Platform::IGPUProbe> makeGPUModelProbe(const Synthetic::Scenario* scenario)
{
    return Synthetic::makeGPUProbe(scenario);
}

ProcessDetailsReaders makeProcessDetailsReaders(const Synthetic::Scenario* scenario)
{
    return {
        .actions = processActionsFor(scenario),
        .environment = brokered(scenario, Synthetic::makeProcessEnvironmentReader),
        .connections = brokered(scenario, Synthetic::makeProcessConnectionsReader),
        .modules = brokered(scenario, Synthetic::makeProcessModulesReader),
        .security = Synthetic::makeProcessSecurityReader(scenario),
        .openFiles = brokered(scenario, Synthetic::makeProcessOpenFilesReader),
        .threads = brokered(scenario, Synthetic::makeProcessThreadsReader),
        .memoryMap = App::makeProcessMemoryMapReader(scenario), // qualified: ADL also finds Synthetic's
    };
}

std::unique_ptr<Platform::IProcessMemoryMapReader> makeProcessMemoryMapReader(const Synthetic::Scenario* scenario)
{
    // Windows' helper reads an elevated or another user's process's map when TaskSmack's own read is refused, and
    // "Authenticate to read" starts it (#3665). Linux has no helper (#3580): a root TaskSmack reads every map itself.
    return brokered(scenario, Synthetic::makeProcessMemoryMapReader);
}

std::unique_ptr<Platform::IProcessActions> makeConnectionsPanelActions(const Synthetic::Scenario* scenario)
{
    return processActionsFor(scenario);
}

std::shared_ptr<Platform::IUserSessions> makeSignedInSessions(const Synthetic::Scenario* scenario)
{
    if (scenario != nullptr)
    {
        return std::make_shared<Platform::UnsupportedUserSessions>();
    }
#ifdef _WIN32
    // Another user's session needs an administrator (#3076): the helper does it. Linux has none (#3580): logind acts
    // for a root TaskSmack, and refuses another user's session to anyone else.
    return std::make_shared<Platform::BrokeredUserSessions>(Platform::makeUserSessions(), sharedPrivilegedBroker());
#else
    return Platform::makeUserSessions();
#endif
}

ServicesPanelPlatform makeServicesPanelPlatform(const Synthetic::Scenario* /*scenario*/)
{
    // No helper does service actions: Windows' doesn't, and Linux has none (#3580).
    return {.probe = Platform::makeServiceProbe(), .actions = Platform::makeServiceActions()};
}

StartupPanelPlatform makeStartupPanelPlatform(const Synthetic::Scenario* /*scenario*/)
{
    return {.probe = Platform::makeStartupProbe(), .actions = Platform::makeStartupActions(), .account = sessionAccountCheck()};
}

std::unique_ptr<Platform::ISystemInfoProbe> makeSystemInfoProbe(const Synthetic::Scenario* /*scenario*/)
{
    // The platform's, in every run: no helper reads any of it since #3580 (a Linux run as root reads the firmware
    // files itself), so the System Information tab and the sensor sampler each take one of these (#3881).
    return Platform::makeSystemInfoProbe();
}

std::shared_ptr<Platform::IHttpsClient> makeHttpsClient(const Synthetic::Scenario* scenario, bool testHookActive)
{
    // No request from a made-up machine or an unattended test run (#3093, #3094): the update check and Look up on
    // VirusTotal aren't offered there.
    if (scenario != nullptr || testHookActive)
    {
        return nullptr;
    }
    return Platform::makeHttpsClient();
}

std::shared_ptr<Platform::IStartAtLogin> makeStartAtLoginEntry(const Synthetic::Scenario* scenario, bool testHookActive)
{
    // Never from a made-up machine or an unattended test run (#3093 part 2): the row isn't offered there.
    if (scenario != nullptr || testHookActive)
    {
        return nullptr;
    }
    return Platform::makeStartAtLogin();
}

std::shared_ptr<SessionAccountCheck> sessionAccountCheck()
{
    // Weak: held by what reads it, so the worker goes with the last of them, before the statics do.
    static std::weak_ptr<SessionAccountCheck> shared;
    std::shared_ptr<SessionAccountCheck> check = shared.lock();
    if (check == nullptr)
    {
        check = std::make_shared<SessionAccountCheck>([] { return Platform::readSessionAccount(); });
        shared = check;
    }
    return check;
}

std::shared_ptr<Platform::ISystemInfoProbe> makeCrashHistoryProbe(const Synthetic::Scenario* scenario)
{
    if (scenario != nullptr)
    {
        return nullptr;
    }
    // Its own probe: the System tab's model keeps its probe to itself, under its read lock.
    return Platform::makeSystemInfoProbe();
}

std::shared_ptr<Platform::IPrivilegedBroker> privilegedBroker(const Synthetic::Scenario* scenario)
{
    return scenario != nullptr ? nullptr : sharedPrivilegedBroker();
}

void Detail::setSharedPrivilegedBrokerForTesting(std::shared_ptr<Platform::IPrivilegedBroker> broker)
{
    brokerForTesting() = std::move(broker);
}

} // namespace App
