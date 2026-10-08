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
    EXPECT_TRUE(probe.capabilities().canEnumerate);

    const auto services = probe.enumerate();
    if (services.empty())
    {
        GTEST_SKIP() << "Service enumeration was denied or unavailable on this machine";
    }

    const auto it = std::ranges::find_if(services, [](const ServiceInfo& s) { return s.name == "EventLog" || s.name == "Winmgmt"; });
    ASSERT_NE(it, services.end());
    EXPECT_FALSE(it->displayName.empty());
    if (it->state == ServiceState::Running)
    {
        EXPECT_NE(it->pid, 0U);
    }

    // A second call reuses the cached configuration and still reports the same services.
    EXPECT_EQ(probe.enumerate().size(), services.size());
}

} // namespace
} // namespace Platform
