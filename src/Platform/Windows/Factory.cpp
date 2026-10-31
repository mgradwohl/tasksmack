#include "Platform/Factory.h"

#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IPathProvider.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessIconSource.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IProcessSecurity.h"
#include "Platform/IProcessThreads.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/ISystemConnections.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/ITaskManagerReplacement.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/TaskManagerReplacement.h"
#include "Platform/TaskManagerReplacementService.h"
#include "Platform/Windows/BrokerHelper.h"
#include "Platform/Windows/HelperLauncher.h"
#include "Platform/Windows/PipeBroker.h"
#include "WinString.h"
#include "WindowsDebugPrivilege.h"
#include "WindowsDiskProbe.h"
#include "WindowsElevation.h"
#include "WindowsEtwNetCollector.h"
#include "WindowsGPUProbe.h"
#include "WindowsHandles.h"
#include "WindowsPathProvider.h"
#include "WindowsPowerProbe.h"
#include "WindowsProcessActions.h"
#include "WindowsProcessConnections.h"
#include "WindowsProcessEnvironmentReader.h"
#include "WindowsProcessIconSource.h"
#include "WindowsProcessModules.h"
#include "WindowsProcessOpenFiles.h"
#include "WindowsProcessProbe.h"
#include "WindowsProcessSecurity.h"
#include "WindowsServiceActions.h"
#include "WindowsServiceProbe.h"
#include "WindowsStartupActions.h"
#include "WindowsStartupProbe.h"
#include "WindowsSystemInfoProbe.h"
#include "WindowsSystemProbe.h"
#include "WindowsTaskManagerReplacement.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
// clang-format on

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform
{

std::unique_ptr<IProcessProbe> makeProcessProbe()
{
    return std::make_unique<WindowsProcessProbe>();
}

std::shared_ptr<IUdpTrafficCollector> makeUdpTrafficCollector()
{
    return std::make_shared<WindowsEtwNetCollector>();
}

std::unique_ptr<IProcessActions> makeProcessActions()
{
    return std::make_unique<WindowsProcessActions>();
}

std::shared_ptr<IPrivilegedBroker> makePrivilegedBroker(void* ownerWindow)
{
    // As on Linux, nothing is offered that can't work (#2550): an elevated TaskSmack needs no helper, a
    // build without TaskSmackHelper.exe has none, and the helper refuses a TaskSmack its user can change
    // (#2572), which a portable or per-user copy is. That is asked now, before any UAC prompt.
    if (const auto available = Windows::HelperLauncher::availability(); !available.has_value())
    {
        spdlog::info("Acting as an administrator isn't offered: {}", available.error());
        return nullptr;
    }
    // A standard user's elevation runs as an administrator, which reads only the user's own processes:
    // no "Read as administrator" is offered there; actions still are (#2550). An administrator's
    // elevation that turns out to run as another account too (Windows 11's Administrator Protection, a
    // credentials prompt answered with another administrator's account) is learned from the helper's
    // first refusal, and then isn't offered either (#2769): until a new helper starts, since a later
    // prompt may be answered with the user's own account, or, under Administrator Protection, where every
    // prompt runs the helper as another account, while TaskSmack runs (#2993).
    const auto lasts = Windows::HelperLauncher::administratorProtection() ? Windows::HelperLauncher::RefusalLasts::ForTheProcess
                                                                          : Windows::HelperLauncher::RefusalLasts::ForItsHelper;
    return std::make_shared<Windows::HelperLauncher::LearnedReadsBroker>(
        std::make_unique<Windows::PipeBroker>(Windows::HelperLauncher::makeLauncher({}, static_cast<HWND>(ownerWindow)),
                                              Windows::HelperLauncher::readsOtherUsers()),
        lasts);
}

std::unique_ptr<IProcessIconSource> makeProcessIconSource()
{
    return std::make_unique<WindowsProcessIconSource>();
}

std::unique_ptr<IProcessEnvironmentReader> makeProcessEnvironmentReader()
{
    // The target's environment block, read out of its PEB with ReadProcessMemory (#2569).
    return std::make_unique<Windows::WindowsProcessEnvironmentReader>();
}

std::unique_ptr<IProcessConnectionsReader> makeProcessConnectionsReader()
{
    // GetExtendedTcpTable/GetExtendedUdpTable owner-PID rows (#1489).
    return std::make_unique<Windows::WindowsProcessConnectionsReader>();
}

std::unique_ptr<IProcessModulesReader> makeProcessModulesReader()
{
    return std::make_unique<Windows::WindowsProcessModulesReader>();
}

std::unique_ptr<ISystemConnectionsReader> makeSystemConnectionsReader()
{
    // The Connections tab (#3066) says it isn't available until the Windows lane adds a reader.
    return std::make_unique<UnsupportedSystemConnectionsReader>();
}

std::unique_ptr<IProcessThreadsReader> makeProcessThreadsReader()
{
    // The Threads section (#3069) is hidden until the Windows lane adds a Toolhelp reader.
    return std::make_unique<UnsupportedProcessThreadsReader>();
}

std::unique_ptr<IProcessOpenFilesReader> makeProcessOpenFilesReader()
{
    return std::make_unique<Windows::WindowsProcessOpenFilesReader>();
}

std::unique_ptr<IProcessSecurityReader> makeProcessSecurityReader()
{
    return std::make_unique<Windows::WindowsProcessSecurityReader>();
}

std::unique_ptr<IStartupProbe> makeStartupProbe()
{
    return std::make_unique<WindowsStartupProbe>();
}

std::unique_ptr<IStartupActions> makeStartupActions()
{
    return std::make_unique<WindowsStartupActions>(Windows::isProcessElevated());
}

std::unique_ptr<ISystemInfoProbe> makeSystemInfoProbe()
{
    return std::make_unique<WindowsSystemInfoProbe>();
}

std::unique_ptr<ISystemProbe> makeSystemProbe()
{
    return std::make_unique<WindowsSystemProbe>();
}

std::unique_ptr<IDiskProbe> makeDiskProbe()
{
    return std::make_unique<WindowsDiskProbe>();
}

std::unique_ptr<IPathProvider> makePathProvider()
{
    return std::make_unique<WindowsPathProvider>();
}

std::unique_ptr<IPowerProbe> makePowerProbe()
{
    return std::make_unique<WindowsPowerProbe>();
}

std::unique_ptr<IGPUProbe> makeGPUProbe()
{
    return std::make_unique<WindowsGPUProbe>();
}

std::unique_ptr<IServiceProbe> makeServiceProbe()
{
    return std::make_unique<WindowsServiceProbe>();
}

std::unique_ptr<IServiceActions> makeServiceActions()
{
    return std::make_unique<WindowsServiceActions>(Windows::isProcessElevated());
}

bool enableDebugPrivilege()
{
    return Windows::enableDebugPrivilegeIfElevated() == Windows::DebugPrivilegeResult::Enabled;
}

std::shared_ptr<ITaskManagerReplacement> makeTaskManagerReplacement(std::shared_ptr<IPrivilegedBroker> broker)
{
    TaskManagerReplacementSetup setup;
    if (const auto image = Windows::BrokerHelper::ownImage(); image.has_value())
    {
        setup.selfPath = WinString::wideToUtf8(*image);
    }
    const std::vector<std::string> folders = Windows::installFolders();
    const std::vector<std::string_view> folderViews(folders.begin(), folders.end());
    setup.installedCopy = TaskManagerReplacement::isInstalledCopy(setup.selfPath, folderViews);
    // Reading HKLM needs no rights: a fresh key handle each time, on the worker that asks.
    setup.readDebugger = []
    {
        Windows::WindowsIfeoRegistry registry;
        return registry.readDebugger();
    };
    setup.fileExists = [](std::string_view path)
    {
        return Windows::fileExists(path);
    };
    if (Windows::isProcessElevated())
    {
        // Elevated, TaskSmack writes the key itself, but only from Program Files: Windows starts the
        // Debugger for whoever opens Task Manager, so it must be a copy only an administrator can change,
        // which the helper checks for itself (BrokerHelper::checkUnchangeableBy()) and Program Files is.
        if (setup.installedCopy)
        {
            setup.direct = std::make_shared<Windows::WindowsIfeoRegistry>();
        }
        else
        {
            setup.unavailableReason =
                "Only a TaskSmack installed in Program Files can replace Task Manager: Windows would start this copy, "
                "which programs running as you could change, whenever anyone opens Task Manager";
        }
    }
    else if (broker != nullptr)
    {
        setup.broker = std::move(broker);
    }
    else
    {
        const auto available = Windows::HelperLauncher::availability();
        setup.unavailableReason = "Replacing Task Manager needs TaskSmack's administrator helper, which this copy can't use" +
                                  (available.has_value() ? std::string() : ": " + available.error());
    }
    return std::make_shared<TaskManagerReplacementService>(std::move(setup));
}

std::vector<std::string> commandLineArguments()
{
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> arguments;
    if (argv != nullptr && count > 1)
    {
        const std::span<LPWSTR> all(argv, static_cast<std::size_t>(count));
        arguments.reserve(all.size() - 1);
        for (const wchar_t* argument : all.subspan(1))
        {
            arguments.push_back(WinString::wideToUtf8(argument));
        }
    }
    LocalFree(static_cast<HLOCAL>(argv));
    return arguments;
}

namespace
{

/// What showRunningTaskSmack() looks for, and finds.
struct RunningWindowSearch
{
    DWORD selfPid = 0;
    HWND found = nullptr;
};

/// Whether @p window is a visible top-level window titled "TaskSmack" of another TaskSmack.exe.
[[nodiscard]] bool isOtherTaskSmackWindow(HWND window, DWORD selfPid)
{
    if (IsWindowVisible(window) == FALSE)
    {
        return false;
    }
    // "TaskSmack", or "TaskSmack (Administrator)" for an elevated one (ShellLayer).
    std::wstring title(64, L'\0');
    const int length = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    const std::wstring_view text(title.data(), static_cast<std::size_t>(std::max(length, 0)));
    if (text != L"TaskSmack" && !text.starts_with(L"TaskSmack ("))
    {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0 || pid == selfPid)
    {
        return false;
    }
    const Windows::UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.valid())
    {
        return false;
    }
    const auto image = Windows::BrokerHelper::imageOf(process.get());
    return image.has_value() && TaskManagerReplacement::isTaskSmackProgram(WinString::wideToUtf8(*image));
}

} // namespace

bool showRunningTaskSmack()
{
    RunningWindowSearch search{.selfPid = GetCurrentProcessId(), .found = nullptr};
    EnumWindows(
        [](HWND window, LPARAM context) -> BOOL
        {
            auto* const state = reinterpret_cast<RunningWindowSearch*>(context); // NOLINT(performance-no-int-to-ptr) - EnumWindows' context
            if (isOtherTaskSmackWindow(window, state->selfPid))
            {
                state->found = window;
                return FALSE; // found: stop
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (search.found == nullptr)
    {
        return false;
    }
    if (IsIconic(search.found) != FALSE)
    {
        ShowWindow(search.found, SW_RESTORE);
    }
    // This process was just started by the user's own key press, so Windows lets it hand the foreground on.
    SetForegroundWindow(search.found);
    return true;
}

} // namespace Platform
