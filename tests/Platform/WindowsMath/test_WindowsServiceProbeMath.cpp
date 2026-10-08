/// @file test_WindowsServiceProbeMath.cpp
/// @brief WindowsServiceProbeMath.h (#800): SCM state and start-type codes, delayed auto-start,
/// service type labels and the svchost group. No Windows header, so these run on every platform.

#include "Platform/IServiceProbe.h"
#include "Platform/Windows/WindowsServiceProbeMath.h"

#include <gtest/gtest.h>

namespace Platform::Windows::ServiceMath
{
namespace
{

TEST(WindowsServiceProbeMathTest, StateCodesMapToStates)
{
    EXPECT_EQ(stateFromCode(1), ServiceState::Stopped);
    EXPECT_EQ(stateFromCode(2), ServiceState::StartPending);
    EXPECT_EQ(stateFromCode(3), ServiceState::StopPending);
    EXPECT_EQ(stateFromCode(4), ServiceState::Running);
    EXPECT_EQ(stateFromCode(5), ServiceState::ContinuePending);
    EXPECT_EQ(stateFromCode(6), ServiceState::PausePending);
    EXPECT_EQ(stateFromCode(7), ServiceState::Paused);
    EXPECT_EQ(stateFromCode(0), ServiceState::Unknown);
    EXPECT_EQ(stateFromCode(99), ServiceState::Unknown);
}

TEST(WindowsServiceProbeMathTest, StartTypeCodesMapToStartTypes)
{
    EXPECT_EQ(startTypeFromCode(0, false), ServiceStartType::Boot);
    EXPECT_EQ(startTypeFromCode(1, false), ServiceStartType::System);
    EXPECT_EQ(startTypeFromCode(2, false), ServiceStartType::Automatic);
    EXPECT_EQ(startTypeFromCode(3, false), ServiceStartType::Manual);
    EXPECT_EQ(startTypeFromCode(4, false), ServiceStartType::Disabled);
    EXPECT_EQ(startTypeFromCode(5, false), ServiceStartType::Unknown);
}

TEST(WindowsServiceProbeMathTest, DelayedAppliesOnlyToAutomatic)
{
    EXPECT_EQ(startTypeFromCode(2, true), ServiceStartType::AutomaticDelayed);
    EXPECT_EQ(startTypeFromCode(3, true), ServiceStartType::Manual);
    EXPECT_EQ(startTypeFromCode(4, true), ServiceStartType::Disabled);
}

TEST(WindowsServiceProbeMathTest, ServiceTypeText)
{
    EXPECT_EQ(serviceTypeText(0x10), "Own process");
    EXPECT_EQ(serviceTypeText(0x20), "Shared process");
    EXPECT_EQ(serviceTypeText(0x110), "Own process");
    EXPECT_EQ(serviceTypeText(0x60), "Shared process (user template)");
    EXPECT_EQ(serviceTypeText(0xE0), "Shared process (user instance)");
    EXPECT_EQ(serviceTypeText(0x1), "Driver");
}

TEST(WindowsServiceProbeMathTest, SvchostGroupFromCommandLine)
{
    EXPECT_EQ(svchostGroup(R"(C:\WINDOWS\system32\svchost.exe -k netsvcs -p)"), "netsvcs");
    EXPECT_EQ(svchostGroup(R"(C:\Windows\System32\SvcHost.exe -K LocalServiceNetworkRestricted)"), "LocalServiceNetworkRestricted");
    EXPECT_EQ(svchostGroup(R"("C:\Program Files\App\service.exe" -k notsvchost)"), "");
    EXPECT_EQ(svchostGroup(R"(C:\WINDOWS\system32\svchost.exe)"), "");
    EXPECT_EQ(svchostGroup(""), "");
}

TEST(WindowsServiceProbeMathTest, ScmOpenFailureDisablesEnumerationWithAReason)
{
    const ServiceCapabilities opened = capabilitiesForScmOpen(0);
    EXPECT_TRUE(opened.canEnumerate);
    EXPECT_TRUE(opened.hasPid);
    EXPECT_TRUE(opened.unavailableReason.empty());

    const ServiceCapabilities denied = capabilitiesForScmOpen(5); // ERROR_ACCESS_DENIED
    EXPECT_FALSE(denied.canEnumerate);
    EXPECT_FALSE(denied.hasPid);
    EXPECT_EQ(denied.unavailableReason, "Access to the Service Control Manager was denied");

    const ServiceCapabilities other = capabilitiesForScmOpen(1722); // RPC_S_SERVER_UNAVAILABLE
    EXPECT_FALSE(other.canEnumerate);
    EXPECT_EQ(other.unavailableReason, "The Service Control Manager could not be opened (error 1722)");
}

} // namespace
} // namespace Platform::Windows::ServiceMath
