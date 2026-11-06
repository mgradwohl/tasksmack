// CI real-system tests of Windows service control (#3490 item 4; tracker #3492): the Services tab's start, stop,
// restart and startup type, through the real WindowsServiceActions and Service Control Manager, which until now had
// only fakes (test_WindowsServiceActions.cpp). Each test installs throwaway services of its own, TaskSmackCiService.exe
// under a unique TaskSmackCi... name, checks the real WindowsServiceProbe sees each state, and stops and deletes them
// on scope exit. Installing a service changes the machine, so they run only where CiRealSystem.h's guard lets them.

#include "Core/Utf8Path.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsServiceActions.h"
#include "Platform/Windows/WindowsServiceActionsMath.h"
#include "Platform/Windows/WindowsServiceProbe.h"
#include "Support/CiRealSystem.h"
#include "Support/CiRealSystemWindows.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsvc.h>
// clang-format on

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef TASKSMACK_CI_SERVICE_PATH
#error "TASKSMACK_CI_SERVICE_PATH must name TaskSmackCiService.exe (tests/CMakeLists.txt)"
#endif

namespace
{

using namespace std::chrono_literals;
using namespace TestSupport::CiRealSystem;
using Platform::ServiceActionResult;
using Platform::ServiceInfo;
using Platform::ServiceStartType;
using Platform::ServiceState;
using Platform::Windows::UniqueServiceHandle;
namespace Math = Platform::Windows::ServiceActionMath;
namespace WinString = Platform::WinString;

/// A throwaway service: TaskSmackCiService.exe registered as an own-process, demand-start service named @p name
/// (LocalSystem, as `sc create` makes one), depending on @p dependsOn when that isn't empty. Stopped, if it runs,
/// and deleted on scope exit.
class ThrowawayService
{
  public:
    ThrowawayService(std::wstring name, std::wstring displayName, const std::wstring& dependsOn = {})
        : m_Name(std::move(name)), m_DisplayName(std::move(displayName))
    {
        m_Manager.reset(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
        if (!m_Manager)
        {
            ADD_FAILURE() << "couldn't open the service manager: " << GetLastError();
            return;
        }
        const std::wstring binary = L"\"" + std::filesystem::path(TASKSMACK_CI_SERVICE_PATH).wstring() + L"\"";
        // A double-NUL-terminated list of one.
        std::wstring dependencies = dependsOn;
        if (!dependencies.empty())
        {
            dependencies.push_back(L'\0');
        }
        m_Service.reset(CreateServiceW(m_Manager.get(),
                                       m_Name.c_str(),
                                       m_DisplayName.c_str(),
                                       SERVICE_ALL_ACCESS,
                                       SERVICE_WIN32_OWN_PROCESS,
                                       SERVICE_DEMAND_START,
                                       SERVICE_ERROR_IGNORE,
                                       binary.c_str(),
                                       nullptr,
                                       nullptr,
                                       dependencies.empty() ? nullptr : dependencies.c_str(),
                                       nullptr,
                                       nullptr));
        if (!m_Service)
        {
            ADD_FAILURE() << "couldn't install the throwaway service: " << GetLastError();
        }
    }
    ~ThrowawayService()
    {
        if (!m_Service)
        {
            return;
        }
        SERVICE_STATUS status{};
        if (ControlService(m_Service.get(), SERVICE_CONTROL_STOP, &status) != FALSE)
        {
            const auto deadline = std::chrono::steady_clock::now() + STARTUP_WAIT;
            while (QueryServiceStatus(m_Service.get(), &status) != FALSE && status.dwCurrentState != SERVICE_STOPPED &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(POLL_INTERVAL);
            }
        }
        DeleteService(m_Service.get());
    }
    ThrowawayService(const ThrowawayService&) = delete;
    ThrowawayService& operator=(const ThrowawayService&) = delete;
    ThrowawayService(ThrowawayService&&) = delete;
    ThrowawayService& operator=(ThrowawayService&&) = delete;

    [[nodiscard]] bool ready() const noexcept
    {
        return static_cast<bool>(m_Service);
    }
    [[nodiscard]] const std::wstring& name() const noexcept
    {
        return m_Name;
    }
    /// The name WindowsServiceActions and ServiceInfo key it by.
    [[nodiscard]] std::string utf8Name() const
    {
        return WinString::wideToUtf8(m_Name);
    }
    [[nodiscard]] std::string utf8DisplayName() const
    {
        return WinString::wideToUtf8(m_DisplayName);
    }

  private:
    std::wstring m_Name;
    std::wstring m_DisplayName;
    UniqueServiceHandle m_Manager;
    UniqueServiceHandle m_Service;
};

/// The service named @p name as the real probe lists it now, its configuration read afresh; one with an empty name
/// when the list can't be read or doesn't have it, which the caller's check of the name then fails on.
[[nodiscard]] ServiceInfo listed(Platform::WindowsServiceProbe& probe, const std::string& name)
{
    probe.forgetCachedConfig();
    Platform::ServiceEnumeration enumeration = probe.enumerate();
    if (!enumeration.ok)
    {
        ADD_FAILURE() << "the probe couldn't list the services: " << enumeration.failureReason;
        return {};
    }
    const auto found = std::ranges::find(enumeration.services, name, &ServiceInfo::name);
    return (found == enumeration.services.end()) ? ServiceInfo{} : *found;
}

/// Whether process @p pid runs TaskSmackCiService.exe.
[[nodiscard]] bool runsTheServiceExe(std::uint32_t pid)
{
    const Platform::Windows::UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    return process && samePath(imageOf(process.get()), std::filesystem::path(TASKSMACK_CI_SERVICE_PATH).wstring());
}

class CiRealSystemServiceTest : public TestSupport::CiRealSystemTest
{};

TEST_F(CiRealSystemServiceTest, StartStopRestartAndStartupTypeGoThroughTheServiceManager)
{
    const ThrowawayService service(L"TaskSmackCi" + uniqueSuffix(), L"TaskSmack CI throwaway service");
    ASSERT_TRUE(service.ready());
    const std::string name = service.utf8Name();
    Platform::WindowsServiceProbe probe;
    Platform::WindowsServiceActions actions(true);
    const Platform::ServiceActionCapabilities capabilities = actions.capabilities();
    EXPECT_TRUE(capabilities.canStart && capabilities.canStop && capabilities.canRestart && capabilities.canSetStartType);
    const std::stop_token never;

    // Installed: stopped, demand start, the program it was registered with.
    ServiceInfo info = listed(probe, name);
    ASSERT_EQ(info.name, name) << "the probe doesn't list the new service";
    EXPECT_EQ(info.state, ServiceState::Stopped);
    EXPECT_EQ(info.startType, ServiceStartType::Manual);
    EXPECT_EQ(info.displayName, service.utf8DisplayName());
    EXPECT_TRUE(samePath(Core::utf8ToPath(info.executablePath).wstring(), std::filesystem::path(TASKSMACK_CI_SERVICE_PATH).wstring()))
        << info.executablePath;

    // Start: running, in a process of the service's program.
    const ServiceActionResult started = actions.start(name, never);
    ASSERT_TRUE(started.ok) << started.message;
    info = listed(probe, name);
    ASSERT_EQ(info.name, name);
    EXPECT_EQ(info.state, ServiceState::Running);
    const std::uint32_t firstPid = info.pid;
    ASSERT_NE(firstPid, 0U);
    EXPECT_TRUE(runsTheServiceExe(firstPid)) << "PID " << firstPid;

    // Starting it again is refused, in the words the result line shows.
    const ServiceActionResult again = actions.start(name, never);
    EXPECT_FALSE(again.ok);
    EXPECT_EQ(again.message, Math::errorText(ERROR_SERVICE_ALREADY_RUNNING));

    // Restart: running again, in a new process.
    const ServiceActionResult restarted = actions.restart(name, never);
    ASSERT_TRUE(restarted.ok) << restarted.message;
    info = listed(probe, name);
    ASSERT_EQ(info.name, name);
    EXPECT_EQ(info.state, ServiceState::Running);
    EXPECT_NE(info.pid, 0U);
    EXPECT_NE(info.pid, firstPid) << "the restart left the first process running";
    EXPECT_TRUE(runsTheServiceExe(info.pid)) << "PID " << info.pid;

    // Stop: stopped, with no process; stopping it again is refused.
    const ServiceActionResult stopped = actions.stop(name, never);
    ASSERT_TRUE(stopped.ok) << stopped.message;
    info = listed(probe, name);
    ASSERT_EQ(info.name, name);
    EXPECT_EQ(info.state, ServiceState::Stopped);
    EXPECT_EQ(info.pid, 0U);
    const ServiceActionResult stoppedAgain = actions.stop(name, never);
    EXPECT_FALSE(stoppedAgain.ok);
    EXPECT_EQ(stoppedAgain.message, Math::errorText(ERROR_SERVICE_NOT_ACTIVE));

    // Each startup type the menu offers, read back by the probe; a disabled service won't start.
    for (const ServiceStartType type : {ServiceStartType::Automatic, ServiceStartType::AutomaticDelayed, ServiceStartType::Disabled})
    {
        const ServiceActionResult set = actions.setStartType(name, type);
        ASSERT_TRUE(set.ok) << set.message;
        info = listed(probe, name);
        ASSERT_EQ(info.name, name);
        EXPECT_EQ(info.startType, type) << "set " << static_cast<int>(type) << ", read " << static_cast<int>(info.startType);
    }
    const ServiceActionResult whileDisabled = actions.start(name, never);
    EXPECT_FALSE(whileDisabled.ok);
    EXPECT_EQ(whileDisabled.message, Math::errorText(ERROR_SERVICE_DISABLED));
    const ServiceActionResult manual = actions.setStartType(name, ServiceStartType::Manual);
    ASSERT_TRUE(manual.ok) << manual.message;
    info = listed(probe, name);
    ASSERT_EQ(info.name, name);
    EXPECT_EQ(info.startType, ServiceStartType::Manual);
    EXPECT_EQ(info.state, ServiceState::Stopped);

    // A type the menu doesn't offer is refused before any call.
    EXPECT_FALSE(actions.setStartType(name, ServiceStartType::Boot).ok);
}

TEST_F(CiRealSystemServiceTest, AStopWithARunningDependentIsRefusedAndNamesIt)
{
    // The dependent is declared second, so it is stopped and deleted first.
    const ThrowawayService base(L"TaskSmackCi" + uniqueSuffix(), L"TaskSmack CI base service");
    ASSERT_TRUE(base.ready());
    const ThrowawayService dependent(L"TaskSmackCiDep" + uniqueSuffix(), L"TaskSmack CI dependent service", base.name());
    ASSERT_TRUE(dependent.ready());
    Platform::WindowsServiceProbe probe;
    Platform::WindowsServiceActions actions(true);
    const std::stop_token never;

    // Starting the dependent starts what it depends on first.
    const ServiceActionResult started = actions.start(dependent.utf8Name(), never);
    ASSERT_TRUE(started.ok) << started.message;
    ServiceInfo baseInfo = listed(probe, base.utf8Name());
    ASSERT_EQ(baseInfo.name, base.utf8Name());
    EXPECT_EQ(baseInfo.state, ServiceState::Running);

    // Stopping the base is refused, naming the dependent: never stopped along with it.
    const ServiceActionResult stopped = actions.stop(base.utf8Name(), never);
    EXPECT_FALSE(stopped.ok);
    const std::vector<std::string> dependents{dependent.utf8DisplayName()};
    EXPECT_EQ(stopped.message, Math::dependentsText(dependents));
    baseInfo = listed(probe, base.utf8Name());
    ASSERT_EQ(baseInfo.name, base.utf8Name());
    EXPECT_EQ(baseInfo.state, ServiceState::Running) << "the refused stop stopped it";
    const ServiceInfo dependentInfo = listed(probe, dependent.utf8Name());
    ASSERT_EQ(dependentInfo.name, dependent.utf8Name());
    EXPECT_EQ(dependentInfo.state, ServiceState::Running);

    // Once the dependent has stopped, the base stops.
    const ServiceActionResult dependentStopped = actions.stop(dependent.utf8Name(), never);
    ASSERT_TRUE(dependentStopped.ok) << dependentStopped.message;
    const ServiceActionResult baseStopped = actions.stop(base.utf8Name(), never);
    EXPECT_TRUE(baseStopped.ok) << baseStopped.message;
}

} // namespace
