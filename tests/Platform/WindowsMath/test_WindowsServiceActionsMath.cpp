/// @file test_WindowsServiceActionsMath.cpp
/// @brief WindowsServiceActionsMath.h (#1577): error codes to messages, the dependents message, and
/// start types to SCM codes, and the SCM wait's clock (#2127). No Windows header, so these run on every platform.

#include "Platform/IServiceProbe.h"
#include "Platform/Windows/WindowsServiceActionsMath.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Platform::Windows::ServiceActionMath
{
namespace
{

TEST(WindowsServiceActionsMathTest, AccessDeniedRequiresAdministrator)
{
    EXPECT_EQ(errorText(5), "Requires administrator");
}

TEST(WindowsServiceActionsMathTest, KnownErrorsHaveReadableMessages)
{
    EXPECT_EQ(errorText(1056), "It is already running");
    EXPECT_EQ(errorText(1062), "It is not running");
    EXPECT_EQ(errorText(1058), "It is disabled; change its startup type first");
    EXPECT_EQ(errorText(1060), "The service no longer exists");
    EXPECT_EQ(errorText(1053), "The service didn't respond in time");
    EXPECT_EQ(errorText(1051), "Other running services depend on it");
    EXPECT_EQ(errorText(1052), "The service doesn't accept that request");
    EXPECT_EQ(errorText(1061), "The service can't accept requests right now");
    EXPECT_EQ(errorText(1068), "A service it depends on failed to start");
    EXPECT_EQ(errorText(1069), "The service couldn't log on as its account");
    EXPECT_EQ(errorText(1072), "The service is marked for deletion");
    EXPECT_EQ(errorText(1115), "Windows is shutting down");
}

TEST(WindowsServiceActionsMathTest, UnknownErrorsShowTheCode)
{
    EXPECT_EQ(errorText(31), "Windows error 31");
}

TEST(WindowsServiceActionsMathTest, DependentsAreListed)
{
    const std::vector<std::string> none;
    EXPECT_EQ(dependentsText(none), "Other running services depend on it");
    const std::vector<std::string> two{"Fax", "Print Workflow"};
    EXPECT_EQ(dependentsText(two), "Stop the services that depend on it first: Fax, Print Workflow");
}

TEST(WindowsServiceActionsMathTest, StartTypesMapToCodes)
{
    EXPECT_EQ(startTypeCode(ServiceStartType::Automatic).value_or(StartTypeCode{.code = 99, .delayed = false}).code, 2U);
    EXPECT_FALSE(startTypeCode(ServiceStartType::Automatic).value_or(StartTypeCode{.code = 99, .delayed = false}).delayed);
    EXPECT_EQ(startTypeCode(ServiceStartType::AutomaticDelayed).value_or(StartTypeCode{.code = 99, .delayed = false}).code, 2U);
    EXPECT_TRUE(startTypeCode(ServiceStartType::AutomaticDelayed).value_or(StartTypeCode{.code = 99, .delayed = false}).delayed);
    EXPECT_EQ(startTypeCode(ServiceStartType::Manual).value_or(StartTypeCode{.code = 99, .delayed = false}).code, 3U);
    EXPECT_EQ(startTypeCode(ServiceStartType::Disabled).value_or(StartTypeCode{.code = 99, .delayed = false}).code, 4U);
    EXPECT_FALSE(startTypeCode(ServiceStartType::Boot).has_value());
    EXPECT_FALSE(startTypeCode(ServiceStartType::System).has_value());
    EXPECT_FALSE(startTypeCode(ServiceStartType::Unknown).has_value());
}

// #2127: the SCM wait. States are SERVICE_STOP_PENDING (3) and SERVICE_STOPPED (1); times in ms.
TEST(WindowsServiceActionsMathTest, StallWindowIsTheHintWithinBounds)
{
    EXPECT_EQ(stallWindowMs(0), WAIT_STALL_MIN_MS);
    EXPECT_EQ(stallWindowMs(2'000), WAIT_STALL_MIN_MS);
    EXPECT_EQ(stallWindowMs(30'000), 30'000U);
    EXPECT_EQ(stallWindowMs(3'600'000), WAIT_TOTAL_MAX_MS);
}

TEST(WindowsServiceActionsMathTest, NoProgressFailsAfterTheStallWindow)
{
    PendingWait wait;
    EXPECT_EQ(wait.observe(0, 3, 0, 0), WaitVerdict::KeepWaiting);
    EXPECT_EQ(wait.observe(9'750, 3, 0, 0), WaitVerdict::KeepWaiting);
    EXPECT_EQ(wait.observe(10'000, 3, 0, 0), WaitVerdict::Stalled);
}

TEST(WindowsServiceActionsMathTest, RisingCheckpointRestartsTheStallWindow)
{
    PendingWait wait;
    EXPECT_EQ(wait.observe(0, 3, 1, 30'000), WaitVerdict::KeepWaiting);
    EXPECT_EQ(wait.observe(25'000, 3, 2, 30'000), WaitVerdict::KeepWaiting);
    EXPECT_EQ(wait.observe(50'000, 3, 2, 30'000), WaitVerdict::KeepWaiting) << "25 s since progress, within the 30 s hint";
    EXPECT_EQ(wait.observe(55'000, 3, 2, 30'000), WaitVerdict::Stalled);
}

TEST(WindowsServiceActionsMathTest, ANewStateIsProgressAndAFallingCheckpointIsNot)
{
    PendingWait wait;
    EXPECT_EQ(wait.observe(0, 3, 5, 0), WaitVerdict::KeepWaiting);
    EXPECT_EQ(wait.observe(8'000, 3, 4, 0), WaitVerdict::KeepWaiting);
    EXPECT_EQ(wait.observe(10'000, 3, 4, 0), WaitVerdict::Stalled) << "a lower checkpoint is no progress";

    PendingWait changed;
    EXPECT_EQ(changed.observe(0, 3, 5, 0), WaitVerdict::KeepWaiting);
    EXPECT_EQ(changed.observe(8'000, 1, 0, 0), WaitVerdict::KeepWaiting) << "a new state restarts the window";
    EXPECT_EQ(changed.observe(17'750, 1, 0, 0), WaitVerdict::KeepWaiting);
    EXPECT_EQ(changed.observe(18'000, 1, 0, 0), WaitVerdict::Stalled);
}

TEST(WindowsServiceActionsMathTest, SteadyProgressStillEndsAtTheOverallCap)
{
    PendingWait wait;
    std::uint32_t checkPoint = 0;
    for (std::uint32_t elapsed = 0; elapsed < WAIT_TOTAL_MAX_MS; elapsed += WAIT_POLL_MS)
    {
        ASSERT_EQ(wait.observe(elapsed, 3, ++checkPoint, 30'000), WaitVerdict::KeepWaiting) << elapsed;
    }
    EXPECT_EQ(wait.observe(WAIT_TOTAL_MAX_MS, 3, ++checkPoint, 30'000), WaitVerdict::TooLong);
}

} // namespace
} // namespace Platform::Windows::ServiceActionMath
