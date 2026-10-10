/// @file test_WindowsServiceActions.cpp
/// @brief WindowsServiceActions (#1577) against a fake Service Control Manager: each action's
/// success, a denied open (Requires administrator), a timed-out wait, a stop refused because of
/// running dependents, already running / not running, start-type changes with delayed auto-start,
/// and a stop request ending the wait within one poll (#1591). No real service is ever started or
/// stopped.

#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/Windows/WindowsServiceActions.h"
#include "Platform/Windows/WindowsServiceActionsMath.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

/// The fake SCM's answers and what it saw, reset per test.
struct FakeScm
{
    DWORD openError = ERROR_SUCCESS;  ///< OpenServiceW fails with this, when set.
    DWORD startError = ERROR_SUCCESS; ///< StartServiceW fails with this, when set.
    DWORD stopError = ERROR_SUCCESS;  ///< ControlService fails with this, when set.
    DWORD configError = ERROR_SUCCESS;
    std::vector<DWORD> states{SERVICE_RUNNING}; ///< QueryServiceStatus answers in turn; the last repeats.
    std::size_t queries = 0;
    std::vector<std::wstring> dependents;
    std::vector<DWORD> accesses; ///< Each OpenServiceW's requested rights.
    int starts = 0;
    int stops = 0;
    int sleeps = 0;
    int handles = 0; ///< Open minus closed.
    DWORD startType = 0;
    int delayedCalls = 0;
    BOOL delayed = FALSE;
    std::stop_source stop;   ///< The actions' token comes from this (token()).
    int stopAfterSleeps = 0; ///< When set, the fake Sleep requests the stop on its call with this count.
};

FakeScm& fake()
{
    static FakeScm instance;
    return instance;
}

SC_HANDLE handle()
{
    static int token = 0;
    ++fake().handles;
    return reinterpret_cast<SC_HANDLE>(&token);
}

SC_HANDLE WINAPI fakeOpenScm(LPCWSTR /*machine*/, LPCWSTR /*database*/, DWORD access)
{
    EXPECT_EQ(access, static_cast<DWORD>(SC_MANAGER_CONNECT));
    return handle();
}

SC_HANDLE WINAPI fakeOpenService(SC_HANDLE /*scm*/, LPCWSTR /*name*/, DWORD access)
{
    fake().accesses.push_back(access);
    if (fake().openError != ERROR_SUCCESS)
    {
        SetLastError(fake().openError);
        return nullptr;
    }
    return handle();
}

BOOL WINAPI fakeClose(SC_HANDLE /*handle*/)
{
    --fake().handles;
    return TRUE;
}

BOOL WINAPI fakeStart(SC_HANDLE /*service*/, DWORD /*argc*/, LPCWSTR* /*argv*/)
{
    ++fake().starts;
    SetLastError(fake().startError);
    return fake().startError == ERROR_SUCCESS ? TRUE : FALSE;
}

BOOL WINAPI fakeControl(SC_HANDLE /*service*/, DWORD control, LPSERVICE_STATUS /*status*/)
{
    EXPECT_EQ(control, static_cast<DWORD>(SERVICE_CONTROL_STOP));
    ++fake().stops;
    SetLastError(fake().stopError);
    return fake().stopError == ERROR_SUCCESS ? TRUE : FALSE;
}

BOOL WINAPI fakeQuery(SC_HANDLE /*service*/, LPSERVICE_STATUS status)
{
    const auto& states = fake().states;
    *status = SERVICE_STATUS{};
    status->dwCurrentState = states[std::min(fake().queries++, states.size() - 1)];
    return TRUE;
}

BOOL WINAPI
fakeEnumDependents(SC_HANDLE /*service*/, DWORD state, LPENUM_SERVICE_STATUSW buffer, DWORD bytes, LPDWORD needed, LPDWORD count)
{
    EXPECT_EQ(state, static_cast<DWORD>(SERVICE_ACTIVE));
    auto& dependents = fake().dependents;
    *needed = static_cast<DWORD>(dependents.size() * sizeof(ENUM_SERVICE_STATUSW));
    if (buffer == nullptr || bytes < *needed)
    {
        SetLastError(ERROR_MORE_DATA);
        return FALSE;
    }
    for (std::size_t i = 0; i < dependents.size(); ++i)
    {
        buffer[i] = ENUM_SERVICE_STATUSW{};
        buffer[i].lpDisplayName = dependents[i].data();
    }
    *count = static_cast<DWORD>(dependents.size());
    return TRUE;
}

BOOL WINAPI fakeChangeConfig(SC_HANDLE /*service*/,
                             DWORD serviceType,
                             DWORD startType,
                             DWORD errorControl,
                             LPCWSTR /*binary*/,
                             LPCWSTR /*group*/,
                             LPDWORD /*tag*/,
                             LPCWSTR /*dependencies*/,
                             LPCWSTR /*account*/,
                             LPCWSTR /*password*/,
                             LPCWSTR /*displayName*/)
{
    EXPECT_EQ(serviceType, static_cast<DWORD>(SERVICE_NO_CHANGE));
    EXPECT_EQ(errorControl, static_cast<DWORD>(SERVICE_NO_CHANGE));
    fake().startType = startType;
    SetLastError(fake().configError);
    return fake().configError == ERROR_SUCCESS ? TRUE : FALSE;
}

BOOL WINAPI fakeChangeConfig2(SC_HANDLE /*service*/, DWORD level, LPVOID info)
{
    EXPECT_EQ(level, static_cast<DWORD>(SERVICE_CONFIG_DELAYED_AUTO_START_INFO));
    ++fake().delayedCalls;
    fake().delayed = static_cast<SERVICE_DELAYED_AUTO_START_INFO*>(info)->fDelayedAutostart;
    return TRUE;
}

void WINAPI fakeSleep(DWORD ms)
{
    EXPECT_EQ(ms, Windows::ServiceActionMath::WAIT_POLL_MS);
    ++fake().sleeps;
    if (fake().stopAfterSleeps != 0 && fake().sleeps == fake().stopAfterSleeps)
    {
        static_cast<void>(fake().stop.request_stop()); // as if TaskSmack closed during this poll's sleep
    }
}

[[nodiscard]] std::stop_token token()
{
    return fake().stop.get_token();
}

class WindowsServiceActionsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeScm{};
    }

    void TearDown() override
    {
        EXPECT_EQ(fake().handles, 0) << "every handle opened is closed";
    }

    [[nodiscard]] static WindowsServiceActions actions()
    {
        return WindowsServiceActions(false,
                                     {.openSCManager = &fakeOpenScm,
                                      .openService = &fakeOpenService,
                                      .closeServiceHandle = &fakeClose,
                                      .startService = &fakeStart,
                                      .controlService = &fakeControl,
                                      .queryServiceStatus = &fakeQuery,
                                      .enumDependentServices = &fakeEnumDependents,
                                      .changeServiceConfig = &fakeChangeConfig,
                                      .changeServiceConfig2 = &fakeChangeConfig2,
                                      .sleep = &fakeSleep});
    }
};

TEST_F(WindowsServiceActionsTest, CapabilitiesAreAllTrueAndCarryElevation)
{
    const ServiceActionCapabilities caps = actions().capabilities();
    EXPECT_TRUE(caps.canStart && caps.canStop && caps.canRestart && caps.canSetStartType);
    EXPECT_FALSE(caps.elevated);
}

TEST_F(WindowsServiceActionsTest, StartWaitsUntilRunning)
{
    fake().states = {SERVICE_START_PENDING, SERVICE_START_PENDING, SERVICE_RUNNING};
    const ServiceActionResult result = actions().start("Spooler", token());
    EXPECT_TRUE(result.ok) << result.message;
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(fake().starts, 1);
    EXPECT_EQ(fake().sleeps, 2);
    ASSERT_EQ(fake().accesses.size(), 1U);
    EXPECT_EQ(fake().accesses[0], static_cast<DWORD>(SERVICE_START | SERVICE_QUERY_STATUS));
}

TEST_F(WindowsServiceActionsTest, DeniedOpenRequiresAdministrator)
{
    fake().openError = ERROR_ACCESS_DENIED;
    EXPECT_EQ(actions().stop("Spooler", token()).message, "Requires administrator");
    EXPECT_EQ(actions().setStartType("Spooler", ServiceStartType::Manual).message, "Requires administrator");
    EXPECT_EQ(fake().stops, 0);
}

TEST_F(WindowsServiceActionsTest, AlreadyRunningAndNotRunningAreReported)
{
    fake().startError = ERROR_SERVICE_ALREADY_RUNNING;
    EXPECT_EQ(actions().start("Spooler", token()).message, "It is already running");
    fake().stopError = ERROR_SERVICE_NOT_ACTIVE;
    EXPECT_EQ(actions().stop("Spooler", token()).message, "It is not running");
}

TEST_F(WindowsServiceActionsTest, StopTimesOutAfterTheBoundedWait)
{
    fake().states = {SERVICE_STOP_PENDING};
    const ServiceActionResult result = actions().stop("Spooler", token());
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.message, "It didn't stop within 10 s");
    EXPECT_EQ(static_cast<std::uint32_t>(fake().sleeps),
              Windows::ServiceActionMath::WAIT_TIMEOUT_MS / Windows::ServiceActionMath::WAIT_POLL_MS);
}

TEST_F(WindowsServiceActionsTest, StartThatFallsBackToStoppedFailsAtOnce)
{
    fake().states = {SERVICE_START_PENDING, SERVICE_STOPPED};
    EXPECT_EQ(actions().start("Spooler", token()).message, "It stopped while starting");
}

TEST_F(WindowsServiceActionsTest, StopWithRunningDependentsNamesThemAndStopsNothing)
{
    fake().stopError = ERROR_DEPENDENT_SERVICES_RUNNING;
    fake().dependents = {L"Fax", L"Print Workflow"};
    const ServiceActionResult result = actions().stop("Spooler", token());
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.message, "Stop the services that depend on it first: Fax, Print Workflow");
    EXPECT_EQ(fake().stops, 1);
    ASSERT_EQ(fake().accesses.size(), 2U);
    EXPECT_EQ(fake().accesses[1], static_cast<DWORD>(SERVICE_ENUMERATE_DEPENDENTS));
}

TEST_F(WindowsServiceActionsTest, RestartStopsThenStarts)
{
    fake().states = {SERVICE_STOP_PENDING, SERVICE_STOPPED, SERVICE_START_PENDING, SERVICE_RUNNING};
    EXPECT_TRUE(actions().restart("Spooler", token()).ok);
    EXPECT_EQ(fake().stops, 1);
    EXPECT_EQ(fake().starts, 1);
}

TEST_F(WindowsServiceActionsTest, RestartDoesNotStartWhenTheStopFails)
{
    fake().openError = ERROR_ACCESS_DENIED;
    EXPECT_EQ(actions().restart("Spooler", token()).message, "Requires administrator");
    EXPECT_EQ(fake().starts, 0);
}

TEST_F(WindowsServiceActionsTest, StopRequestMidWaitReturnsCancelledAtTheNextPoll)
{
    fake().states = {SERVICE_STOP_PENDING}; // never stops: without the stop request this waits out 10 s
    fake().stopAfterSleeps = 3;
    const ServiceActionResult result = actions().stop("Spooler", token());
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(result.message, ServiceActionResult::stopRequested().message);
    EXPECT_EQ(fake().stops, 1) << "the stop request was sent, and is not undone";
    EXPECT_EQ(fake().sleeps, 3) << "no further poll after the one the stop was requested in";
    EXPECT_EQ(fake().queries, 4U) << "the state is read once more after the stop request";
}

TEST_F(WindowsServiceActionsTest, StateReachedAtTheCancellingPollStillSucceeds)
{
    fake().states = {SERVICE_START_PENDING, SERVICE_START_PENDING, SERVICE_RUNNING};
    fake().stopAfterSleeps = 2;
    const ServiceActionResult result = actions().start("Spooler", token());
    EXPECT_TRUE(result.ok) << result.message;
    EXPECT_FALSE(result.cancelled);
}

TEST_F(WindowsServiceActionsTest, StopRequestedBeforeTheActionSendsNothing)
{
    static_cast<void>(fake().stop.request_stop());
    EXPECT_TRUE(actions().start("Spooler", token()).cancelled);
    EXPECT_TRUE(actions().stop("Spooler", token()).cancelled);
    EXPECT_TRUE(actions().restart("Spooler", token()).cancelled);
    EXPECT_TRUE(fake().accesses.empty()) << "no service was even opened";
    EXPECT_EQ(fake().starts, 0);
    EXPECT_EQ(fake().stops, 0);
    EXPECT_EQ(fake().queries, 0U);
}

TEST_F(WindowsServiceActionsTest, RestartStoppedOnRequestDoesNotStartAgain)
{
    // The stop request arrives while the service stops; it does stop, but is then left stopped.
    fake().states = {SERVICE_STOP_PENDING, SERVICE_STOPPED};
    fake().stopAfterSleeps = 1;
    const ServiceActionResult result = actions().restart("Spooler", token());
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(fake().stops, 1);
    EXPECT_EQ(fake().starts, 0);
}

TEST_F(WindowsServiceActionsTest, StartTypesSetTheCodeAndDelayedFlag)
{
    EXPECT_TRUE(actions().setStartType("Spooler", ServiceStartType::AutomaticDelayed).ok);
    EXPECT_EQ(fake().startType, static_cast<DWORD>(SERVICE_AUTO_START));
    EXPECT_EQ(fake().delayed, TRUE);
    EXPECT_EQ(fake().accesses.back(), static_cast<DWORD>(SERVICE_CHANGE_CONFIG));

    EXPECT_TRUE(actions().setStartType("Spooler", ServiceStartType::Automatic).ok);
    EXPECT_EQ(fake().delayed, FALSE);
    EXPECT_EQ(fake().delayedCalls, 2);

    EXPECT_TRUE(actions().setStartType("Spooler", ServiceStartType::Disabled).ok);
    EXPECT_EQ(fake().startType, static_cast<DWORD>(SERVICE_DISABLED));
    EXPECT_TRUE(actions().setStartType("Spooler", ServiceStartType::Manual).ok);
    EXPECT_EQ(fake().startType, static_cast<DWORD>(SERVICE_DEMAND_START));
    EXPECT_EQ(fake().delayedCalls, 2) << "delayed auto-start is set only for an automatic service";
}

TEST_F(WindowsServiceActionsTest, UnsettableStartTypeAndConfigFailureAreRefused)
{
    EXPECT_FALSE(actions().setStartType("Spooler", ServiceStartType::Boot).ok);
    EXPECT_TRUE(fake().accesses.empty());
    fake().configError = ERROR_ACCESS_DENIED;
    EXPECT_EQ(actions().setStartType("Spooler", ServiceStartType::Manual).message, "Requires administrator");
}

TEST(UnsupportedServiceActionsTest, RefusesEverything)
{
    UnsupportedServiceActions actions;
    const ServiceActionCapabilities caps = actions.capabilities();
    EXPECT_FALSE(caps.canStart || caps.canStop || caps.canRestart || caps.canSetStartType);
    EXPECT_FALSE(actions.start("x", std::stop_token{}).ok);
    EXPECT_FALSE(actions.setStartType("x", ServiceStartType::Manual).ok);
}

} // namespace
} // namespace Platform
