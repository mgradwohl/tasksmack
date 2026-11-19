/// @file test_WindowsServiceProbe.cpp
/// @brief Smoke test of the real WindowsServiceProbe (#800): the list holds a service every Windows
/// install runs, with its configuration read (start type, command line, account), cached, and read
/// again once the cache is dropped. Skipped where the
/// Service Control Manager can't be opened or listed. Also the open of the manager (#4184) against
/// fake SCM calls: none in the constructor, a passing failure retried, a denial final.

#include "Platform/IServiceProbe.h"
#include "Platform/NativeId.h"
#include "Platform/Windows/WindowsNativeIds.h"
#include "Platform/Windows/WindowsServiceConfig.h"
#include "Platform/Windows/WindowsServiceProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

[[nodiscard]] const ServiceInfo* findEventLog(const std::vector<ServiceInfo>& services)
{
    const auto it = std::ranges::find_if(services, [](const ServiceInfo& s) { return s.name == "EventLog"; });
    return it != services.end() ? &*it : nullptr;
}

/// The answers the fake SCM calls give, set per test; they are plain function pointers.
struct FakeScmOpen
{
    int opens = 0;
    int closes = 0;
    int lists = 0;
    DWORD failWith = ERROR_SUCCESS; ///< The error the next opens fail with; success when ERROR_SUCCESS.
    int failuresLeft = 0;           ///< How many opens fail before the rest succeed.
};

FakeScmOpen& fakeOpen()
{
    static FakeScmOpen instance;
    return instance;
}

SC_HANDLE WINAPI fakeOpenSCManager(LPCWSTR /*machine*/, LPCWSTR /*database*/, DWORD /*access*/)
{
    FakeScmOpen& fake = fakeOpen();
    ++fake.opens;
    if (fake.failuresLeft > 0)
    {
        --fake.failuresLeft;
        SetLastError(fake.failWith);
        return nullptr;
    }
    static int token = 0; // any non-null value: the fakes never dereference it
    return reinterpret_cast<SC_HANDLE>(&token);
}

BOOL WINAPI fakeCloseHandle(SC_HANDLE /*handle*/)
{
    ++fakeOpen().closes;
    return TRUE;
}

BOOL WINAPI fakeEnumServices(SC_HANDLE /*scm*/,
                             SC_ENUM_TYPE /*infoLevel*/,
                             DWORD /*type*/,
                             DWORD /*state*/,
                             LPBYTE /*buffer*/,
                             DWORD /*bytes*/,
                             LPDWORD needed,
                             LPDWORD count,
                             LPDWORD /*resume*/,
                             LPCWSTR /*group*/)
{
    ++fakeOpen().lists;
    *needed = 0;
    *count = 0;
    return TRUE; // an empty list
}

[[nodiscard]] Windows::ServiceConfigFunctions fakeApi()
{
    Windows::ServiceConfigFunctions api;
    api.openSCManager = &fakeOpenSCManager;
    api.closeServiceHandle = &fakeCloseHandle;
    api.enumServicesStatusEx = &fakeEnumServices;
    return api;
}

TEST(WindowsServiceProbeOpenTest, ConstructorMakesNoServiceManagerCall)
{
    fakeOpen() = {};
    const WindowsServiceProbe probe(fakeApi());
    EXPECT_EQ(fakeOpen().opens, 0);
    // Optimistic until the first enumerate() has run.
    EXPECT_TRUE(probe.capabilities().canEnumerate);
    EXPECT_EQ(fakeOpen().opens, 0);
}

TEST(WindowsServiceProbeOpenTest, FirstEnumerateOpensTheManagerOnceAndKeepsIt)
{
    fakeOpen() = {};
    {
        WindowsServiceProbe probe(fakeApi());
        EXPECT_TRUE(probe.enumerate().ok);
        EXPECT_TRUE(probe.enumerate().ok);
        EXPECT_EQ(fakeOpen().opens, 1);
        EXPECT_EQ(fakeOpen().lists, 2);
    }
    EXPECT_EQ(fakeOpen().closes, 1);
}

TEST(WindowsServiceProbeOpenTest, APassingFailureIsRetriedOnTheNextEnumerate)
{
    fakeOpen() = {};
    fakeOpen().failWith = RPC_S_SERVER_UNAVAILABLE;
    fakeOpen().failuresLeft = 1;
    WindowsServiceProbe probe(fakeApi());

    const ServiceEnumeration first = probe.enumerate();
    EXPECT_FALSE(first.ok);
    EXPECT_NE(first.failureReason.find("could not be opened"), std::string::npos) << first.failureReason;
    EXPECT_EQ(fakeOpen().opens, 1);
    EXPECT_EQ(fakeOpen().lists, 0);
    // Still optimistic: nothing final happened.
    EXPECT_TRUE(probe.capabilities().canEnumerate);

    EXPECT_TRUE(probe.enumerate().ok);
    EXPECT_EQ(fakeOpen().opens, 2);
    EXPECT_EQ(fakeOpen().lists, 1);
}

TEST(WindowsServiceProbeOpenTest, AccessDeniedStaysRefused)
{
    fakeOpen() = {};
    fakeOpen().failWith = ERROR_ACCESS_DENIED;
    fakeOpen().failuresLeft = 1; // a later open would succeed, but none is attempted
    WindowsServiceProbe probe(fakeApi());

    const ServiceEnumeration first = probe.enumerate();
    EXPECT_FALSE(first.ok);
    EXPECT_EQ(first.failureReason, "Access to the Service Control Manager was denied");
    EXPECT_FALSE(probe.capabilities().canEnumerate);

    EXPECT_FALSE(probe.enumerate().ok);
    EXPECT_FALSE(probe.enumerate().ok);
    EXPECT_EQ(fakeOpen().opens, 1);
}

TEST(WindowsServiceProbeTest, EnumeratesEventLogWithItsConfiguration)
{
    WindowsServiceProbe probe;
    if (!probe.capabilities().canEnumerate)
    {
        GTEST_SKIP() << probe.capabilities().unavailableReason;
    }
    const auto result = probe.enumerate();
    if (!result.ok)
    {
        GTEST_SKIP() << "Service enumeration failed: " << result.failureReason;
    }
    EXPECT_TRUE(result.failureReason.empty());

    const ServiceInfo* eventLog = findEventLog(result.services);
    ASSERT_NE(eventLog, nullptr);
    EXPECT_FALSE(eventLog->displayName.empty());
    if (eventLog->state == ServiceState::Running)
    {
        EXPECT_NE(eventLog->pid, 0U);
    }
    // Configuration is readable without elevation for this service: an empty command line or an
    // Unknown start type means OpenServiceW failed (e.g. the SCM opened without SC_MANAGER_CONNECT).
    EXPECT_NE(eventLog->startType, ServiceStartType::Unknown);
    EXPECT_TRUE(eventLog->binaryPath.contains("svchost.exe")) << eventLog->binaryPath;
    EXPECT_FALSE(eventLog->account.empty());
    EXPECT_FALSE(eventLog->group.empty());
    // #1800: the program the command line runs, for the row's icon.
    EXPECT_TRUE(eventLog->executablePath.ends_with("svchost.exe") || eventLog->executablePath.ends_with("SVCHOST.EXE"))
        << eventLog->executablePath;

    // A second call reuses the cached configuration. The list itself may change between calls
    // (services come and go), so only a service that is always there is compared.
    const auto again = probe.enumerate();
    ASSERT_TRUE(again.ok);
    const ServiceInfo* eventLogAgain = findEventLog(again.services);
    ASSERT_NE(eventLogAgain, nullptr);
    EXPECT_EQ(eventLogAgain->binaryPath, eventLog->binaryPath);
    EXPECT_EQ(eventLogAgain->startType, eventLog->startType);

    // After a service action the cache is dropped (#1577), and the next call reads the configuration
    // afresh: the same, as nothing changed it.
    probe.forgetCachedConfig();
    const auto reread = probe.enumerate();
    ASSERT_TRUE(reread.ok);
    const ServiceInfo* eventLogReread = findEventLog(reread.services);
    ASSERT_NE(eventLogReread, nullptr);
    EXPECT_EQ(eventLogReread->binaryPath, eventLog->binaryPath);
    EXPECT_EQ(eventLogReread->startType, eventLog->startType);
    EXPECT_EQ(eventLogReread->account, eventLog->account);
}

/// Two services whose short names read the same once shown as UTF-8 (#4304): "Up<U+D800>date", with a lone
/// surrogate, and "Up<U+FFFD>date".
[[nodiscard]] std::vector<std::wstring>& twinNames()
{
    static std::vector<std::wstring> names{std::wstring(L"Up") + static_cast<wchar_t>(0xD800) + L"date", L"Up\uFFFDdate"};
    return names;
}

BOOL WINAPI fakeEnumTwins(SC_HANDLE /*scm*/,
                          SC_ENUM_TYPE /*infoLevel*/,
                          DWORD /*type*/,
                          DWORD /*state*/,
                          LPBYTE buffer,
                          DWORD bytes,
                          LPDWORD needed,
                          LPDWORD count,
                          LPDWORD /*resume*/,
                          LPCWSTR /*group*/)
{
    std::vector<std::wstring>& names = twinNames();
    *needed = static_cast<DWORD>(names.size() * sizeof(ENUM_SERVICE_STATUS_PROCESSW));
    *count = 0;
    if (buffer == nullptr || bytes < *needed)
    {
        SetLastError(ERROR_MORE_DATA);
        return FALSE;
    }
    auto* entries = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer);
    for (std::size_t index = 0; index < names.size(); ++index)
    {
        entries[index] = ENUM_SERVICE_STATUS_PROCESSW{};
        entries[index].lpServiceName = names[index].data();
        entries[index].lpDisplayName = names[index].data();
        entries[index].ServiceStatusProcess.dwCurrentState = SERVICE_STOPPED;
    }
    *count = static_cast<DWORD>(names.size());
    return TRUE;
}

SC_HANDLE WINAPI noService(SC_HANDLE /*scm*/, LPCWSTR /*name*/, DWORD /*access*/)
{
    SetLastError(ERROR_ACCESS_DENIED);
    return nullptr; // no configuration: the list alone is under test
}

TEST(WindowsServiceProbeOpenTest, TwoServicesShownAlikeGetTwoIdsForTheirExactNames)
{
    fakeOpen() = {};
    Windows::ServiceConfigFunctions api = fakeApi();
    api.enumServicesStatusEx = &fakeEnumTwins;
    api.openService = &noService;
    const auto ids = std::make_shared<Windows::ServiceIdTable>();
    WindowsServiceProbe probe(api, ids);

    const ServiceEnumeration first = probe.enumerate();
    ASSERT_TRUE(first.ok) << first.failureReason;
    ASSERT_EQ(first.services.size(), 2U);
    EXPECT_EQ(first.services[0].name, first.services[1].name) << "the same display text";
    EXPECT_NE(first.services[0].nativeId, NativeId::None);
    EXPECT_NE(first.services[0].nativeId, first.services[1].nativeId);
    EXPECT_EQ(ids->resolve(first.services[0].nativeId), twinNames()[0]) << "the lone surrogate kept";
    EXPECT_EQ(ids->resolve(first.services[1].nativeId), twinNames()[1]);

    // Listed again, each keeps its id.
    const ServiceEnumeration second = probe.enumerate();
    ASSERT_EQ(second.services.size(), 2U);
    EXPECT_EQ(second.services[0].nativeId, first.services[0].nativeId);
    EXPECT_EQ(second.services[1].nativeId, first.services[1].nativeId);
}

} // namespace
} // namespace Platform
