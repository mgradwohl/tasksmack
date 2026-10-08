/// @file test_WindowsServiceProbe.cpp
/// @brief Smoke test of the real WindowsServiceProbe (#800): the list holds a service every Windows
/// install runs, with its configuration read (start type, command line, account). Skipped where the
/// Service Control Manager can't be opened or listed.

#include "Platform/IServiceProbe.h"
#include "Platform/Windows/WindowsServiceProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
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

    // A second call reuses the cached configuration. The list itself may change between calls
    // (services come and go), so only a service that is always there is compared.
    const auto again = probe.enumerate();
    ASSERT_TRUE(again.ok);
    const ServiceInfo* eventLogAgain = findEventLog(again.services);
    ASSERT_NE(eventLogAgain, nullptr);
    EXPECT_EQ(eventLogAgain->binaryPath, eventLog->binaryPath);
    EXPECT_EQ(eventLogAgain->startType, eventLog->startType);
}

} // namespace
} // namespace Platform
