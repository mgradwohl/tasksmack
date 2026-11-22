/// @file test_WindowsServiceProbeMath.cpp
/// @brief WindowsServiceProbeMath.h (#800): SCM state and start-type codes, delayed auto-start,
/// service type labels and the svchost group. No Windows header, so these run on every platform.

#include "Platform/IServiceProbe.h"
#include "Platform/Windows/WindowsServiceProbeMath.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

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

// #4755: the well-known accounts read as services.msc words them, whatever casing the installer wrote, and a
// per-user service, which names no account, runs as the signed-in user. Any other account is kept as written.
TEST(WindowsServiceProbeMathTest, ServiceAccountsReadAsServicesMscWordsThem)
{
    constexpr std::uint32_t SHARED = 0x20;
    EXPECT_EQ(serviceAccountText("LocalSystem", SHARED), "Local System");
    EXPECT_EQ(serviceAccountText("localSystem", SHARED), "Local System") << "BDESVC's casing";
    EXPECT_EQ(serviceAccountText(".\\LocalSystem", SHARED), "Local System");
    EXPECT_EQ(serviceAccountText("NT AUTHORITY\\LocalService", SHARED), "Local Service");
    EXPECT_EQ(serviceAccountText("NT Authority\\LocalService", SHARED), "Local Service");
    EXPECT_EQ(serviceAccountText("NT Authority\\NetworkService", SHARED), "Network Service");
    EXPECT_EQ(serviceAccountText("nt authority\\networkservice", 0x10), "Network Service");

    EXPECT_EQ(serviceAccountText("", 0xE0), "Signed-in user") << "a per-user instance (CDPUserSvc_c9efb1)";
    EXPECT_EQ(serviceAccountText("", 0x60), "Signed-in user") << "its template";
    EXPECT_EQ(serviceAccountText("", SHARED), "") << "not read: no account to name";
    EXPECT_EQ(serviceAccountText("NT AUTHORITY\\", SHARED), "NT AUTHORITY\\");
    EXPECT_EQ(serviceAccountText(".\\svc-backup", 0x10), ".\\svc-backup");
    EXPECT_EQ(serviceAccountText("NT SERVICE\\MSSQLSERVER", 0x10), "NT SERVICE\\MSSQLSERVER");
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
    EXPECT_EQ(svchostGroup(R"("C:\Windows\System32\svchost.exe" -k netsvcs -p)"), "netsvcs");
    EXPECT_EQ(svchostGroup(R"(C:\WINDOWS\SYSTEM32\SVCHOST.EXE -k DcomLaunch)"), "DcomLaunch");
    EXPECT_EQ(svchostGroup(R"(C:\Program Files\Host Dir\svchost.exe -k grouped)"), "grouped");
    EXPECT_EQ(svchostGroup(R"(C:\Tools\my-svchost.exe -k worker)"), "");
    EXPECT_EQ(svchostGroup(R"("C:\Tools\svchost.exe.bak" -k worker)"), "");
    EXPECT_EQ(svchostGroup(R"(C:\Tools\worker.exe C:\Windows\svchost.exe -k worker)"), "");
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

TEST(WindowsServiceProbeMathTest, TabsSeparateArgumentsLikeSpaces)
{
    EXPECT_EQ(svchostGroup("svchost.exe\t-k\tnetsvcs"), "netsvcs");
    EXPECT_EQ(svchostGroup("\tC:\\Windows\\System32\\svchost.exe\t-k\tnetsvcs\t-p"), "netsvcs");
    EXPECT_EQ(svchostGroup("\"C:\\Windows\\System32\\svchost.exe\"\t-k netsvcs"), "netsvcs");
    EXPECT_EQ(svchostGroup("C:\\Windows\\System32\\svchost.exe \t -k\t \tLocalService  -p"), "LocalService");
    EXPECT_EQ(svchostGroup("C:\\Tools\\my-svchost.exe\t-k\tworker"), "");

    const auto [program, rest] = splitProgram("\"C:\\A B\\x.exe\"\t-a");
    EXPECT_EQ(program, "C:\\A B\\x.exe");
    EXPECT_EQ(rest, "\t-a");
    EXPECT_EQ(splitProgram("C:\\A B\\x.exe\t-a").first, "C:\\A B\\x.exe");
    EXPECT_EQ(splitProgram("tool\t-a").first, "tool");
}

TEST(WindowsServiceProbeMathTest, SplitProgramHandlesQuotesAndUnquotedSpaces)
{
    EXPECT_EQ(splitProgram(R"("C:\A B\x.exe" -a)").first, R"(C:\A B\x.exe)");
    EXPECT_EQ(splitProgram(R"(C:\A B\x.exe -a)").first, R"(C:\A B\x.exe)");
    EXPECT_EQ(splitProgram(R"(C:\A B\x.exe -a)").second, " -a");
    EXPECT_EQ(splitProgram("tool -a").first, "tool");
    EXPECT_EQ(splitProgram("   ").first, "");
}

TEST(WindowsServiceProbeMathTest, AnUnterminatedQuoteRunsToTheEndOfTheLine)
{
    // No closing quote: the rest of the line is the program, and there are no arguments.
    const auto [program, rest] = splitProgram(R"(  "C:\A B\x.exe -a)");
    EXPECT_EQ(program, R"(C:\A B\x.exe -a)");
    EXPECT_EQ(rest, "");
}

TEST(WindowsServiceProbeMathTest, AnUnquotedProgramWithNoSeparatorIsTheWholeLine)
{
    const auto [program, rest] = splitProgram("svchost");
    EXPECT_EQ(program, "svchost");
    EXPECT_EQ(rest, "");
}

TEST(WindowsServiceProbeMathTest, AServiceTypeWithNoProcessOrDriverBitIsOther)
{
    EXPECT_EQ(serviceTypeText(0x0), "Other");
    EXPECT_EQ(serviceTypeText(0x4), "Other"); // SERVICE_ADAPTER
    EXPECT_EQ(serviceTypeText(0x40), "Other (user template)");
    EXPECT_EQ(serviceTypeText(0xC0), "Other (user instance)");
}

TEST(WindowsServiceProbeMathTest, ConfigIsReadOnceThenEveryRefreshWhateverTheOutcome)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point readAt{std::chrono::seconds(1000)};
    // Never attempted: read now.
    EXPECT_TRUE(shouldRefreshConfig({}, readAt, false));
    // Attempted (successfully or denied): not again until CONFIG_REFRESH has passed.
    EXPECT_FALSE(shouldRefreshConfig(readAt, readAt, true));
    EXPECT_FALSE(shouldRefreshConfig(readAt, readAt + CONFIG_REFRESH - std::chrono::seconds(1), true));
    EXPECT_TRUE(shouldRefreshConfig(readAt, readAt + CONFIG_REFRESH, true));
}

TEST(WindowsServiceProbeMathTest, ScmFailureReasons)
{
    EXPECT_EQ(scmFailureReason(5, "list the services"), "Access to the Service Control Manager was denied");
    EXPECT_EQ(scmFailureReason(1722, "list the services"), "The Service Control Manager could not list the services (error 1722)");
}

} // namespace
} // namespace Platform::Windows::ServiceMath
