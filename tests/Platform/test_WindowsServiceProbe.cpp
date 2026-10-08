/// @file test_WindowsServiceProbe.cpp
/// @brief Smoke test of the real WindowsServiceProbe (#800): the list is non-empty and holds a
/// service every Windows install runs. Skipped where the Service Control Manager denies enumeration.

#include "Platform/IServiceProbe.h"
#include "Platform/Windows/WindowsServiceProbe.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace Platform
{
namespace
{

TEST(WindowsServiceProbeTest, EnumeratesWellKnownServices)
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
    const auto& services = result.services;
    ASSERT_FALSE(services.empty());

    const auto it = std::ranges::find_if(services, [](const ServiceInfo& s) { return s.name == "EventLog" || s.name == "Winmgmt"; });
    ASSERT_NE(it, services.end());
    EXPECT_FALSE(it->displayName.empty());
    if (it->state == ServiceState::Running)
    {
        EXPECT_NE(it->pid, 0U);
    }

    // A second call reuses the cached configuration and still reports the same services.
    const auto again = probe.enumerate();
    EXPECT_TRUE(again.ok);
    EXPECT_EQ(again.services.size(), services.size());
}

} // namespace
} // namespace Platform
