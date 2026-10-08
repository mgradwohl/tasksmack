/// @file test_WindowsServiceConfig.cpp
/// @brief Platform::Windows::readServiceConfig() (#800) against fake SCM calls: every configuration
/// field from fixed answers, delayed auto-start, a denied open and a denied query, the size-query
/// protocol, and the service handle closed exactly once.

#include "Platform/IServiceProbe.h"
#include "Platform/Windows/WindowsServiceConfig.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace Platform::Windows
{
namespace
{

/// The answers the fakes give, set per test.
struct FakeScm
{
    bool denyOpen = false;
    bool denyConfig = false;
    DWORD startType = SERVICE_AUTO_START;
    BOOL delayed = FALSE;
    std::wstring binaryPath = LR"(C:\WINDOWS\system32\svchost.exe -k netsvcs -p)";
    std::wstring account = L"LocalSystem";
    std::wstring description = L"Keeps the widgets turning.";
    int opens = 0;
    int closes = 0;
};

/// The fakes are plain function pointers, so they read their answers from here.
FakeScm& fake()
{
    static FakeScm instance;
    return instance;
}

// Any non-null value: the fakes never dereference it.
SC_HANDLE fakeServiceHandle()
{
    static int token = 0;
    return reinterpret_cast<SC_HANDLE>(&token);
}

SC_HANDLE WINAPI fakeOpenService(SC_HANDLE /*scm*/, LPCWSTR /*name*/, DWORD access)
{
    EXPECT_EQ(access, static_cast<DWORD>(SERVICE_QUERY_CONFIG));
    if (fake().denyOpen)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    ++fake().opens;
    return fakeServiceHandle();
}

BOOL WINAPI fakeQueryServiceConfig(SC_HANDLE /*service*/, LPQUERY_SERVICE_CONFIGW config, DWORD bytes, LPDWORD needed)
{
    if (fake().denyConfig)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    *needed = sizeof(QUERY_SERVICE_CONFIGW);
    if (config == nullptr || bytes < sizeof(QUERY_SERVICE_CONFIGW))
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    *config = QUERY_SERVICE_CONFIGW{};
    config->dwServiceType = SERVICE_WIN32_SHARE_PROCESS;
    config->dwStartType = fake().startType;
    config->lpBinaryPathName = fake().binaryPath.data();
    config->lpServiceStartName = fake().account.data();
    return TRUE;
}

BOOL WINAPI fakeQueryServiceConfig2(SC_HANDLE /*service*/, DWORD level, LPBYTE buffer, DWORD bytes, LPDWORD needed)
{
    if (level == SERVICE_CONFIG_DELAYED_AUTO_START_INFO)
    {
        *needed = sizeof(SERVICE_DELAYED_AUTO_START_INFO);
        const SERVICE_DELAYED_AUTO_START_INFO info{.fDelayedAutostart = fake().delayed};
        std::memcpy(buffer, &info, sizeof(info));
        return bytes >= sizeof(info) ? TRUE : FALSE;
    }
    if (level == SERVICE_CONFIG_DESCRIPTION)
    {
        *needed = sizeof(SERVICE_DESCRIPTIONW);
        if (buffer == nullptr || bytes < sizeof(SERVICE_DESCRIPTIONW))
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        const SERVICE_DESCRIPTIONW description{.lpDescription = fake().description.data()};
        std::memcpy(buffer, &description, sizeof(description));
        return TRUE;
    }
    return FALSE;
}

BOOL WINAPI fakeCloseServiceHandle(SC_HANDLE handle)
{
    EXPECT_EQ(handle, fakeServiceHandle());
    ++fake().closes;
    return TRUE;
}

constexpr ServiceConfigFunctions FAKE_API{
    .openService = &fakeOpenService,
    .queryServiceConfig = &fakeQueryServiceConfig,
    .queryServiceConfig2 = &fakeQueryServiceConfig2,
    .closeServiceHandle = &fakeCloseServiceHandle,
};

class WindowsServiceConfigTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeScm{};
    }
};

TEST_F(WindowsServiceConfigTest, ReadsEveryField)
{
    const ServiceConfig config = readServiceConfig(FAKE_API, nullptr, L"Widgets");
    EXPECT_EQ(config.startType, ServiceStartType::Automatic);
    EXPECT_EQ(config.binaryPath, R"(C:\WINDOWS\system32\svchost.exe -k netsvcs -p)");
    EXPECT_EQ(config.account, "LocalSystem");
    EXPECT_EQ(config.description, "Keeps the widgets turning.");
    EXPECT_EQ(config.group, "netsvcs");
    EXPECT_EQ(fake().opens, 1);
    EXPECT_EQ(fake().closes, 1);
}

TEST_F(WindowsServiceConfigTest, DelayedAutoStartAndOtherStartTypes)
{
    fake().delayed = TRUE;
    EXPECT_EQ(readServiceConfig(FAKE_API, nullptr, L"Widgets").startType, ServiceStartType::AutomaticDelayed);
    fake().startType = SERVICE_DISABLED;
    EXPECT_EQ(readServiceConfig(FAKE_API, nullptr, L"Widgets").startType, ServiceStartType::Disabled);
    fake().startType = SERVICE_DEMAND_START;
    fake().delayed = FALSE;
    EXPECT_EQ(readServiceConfig(FAKE_API, nullptr, L"Widgets").startType, ServiceStartType::Manual);
}

TEST_F(WindowsServiceConfigTest, DeniedOpenLeavesEveryFieldEmpty)
{
    fake().denyOpen = true;
    const ServiceConfig config = readServiceConfig(FAKE_API, nullptr, L"Widgets");
    EXPECT_EQ(config.startType, ServiceStartType::Unknown);
    EXPECT_TRUE(config.binaryPath.empty());
    EXPECT_TRUE(config.account.empty());
    EXPECT_TRUE(config.description.empty());
    EXPECT_EQ(fake().closes, 0); // nothing opened, nothing closed
}

TEST_F(WindowsServiceConfigTest, DeniedConfigQueryKeepsTheDescription)
{
    fake().denyConfig = true;
    const ServiceConfig config = readServiceConfig(FAKE_API, nullptr, L"Widgets");
    EXPECT_EQ(config.startType, ServiceStartType::Unknown);
    EXPECT_TRUE(config.binaryPath.empty());
    EXPECT_TRUE(config.account.empty());
    EXPECT_EQ(config.description, "Keeps the widgets turning.");
    EXPECT_EQ(fake().closes, 1);
}

} // namespace
} // namespace Platform::Windows
