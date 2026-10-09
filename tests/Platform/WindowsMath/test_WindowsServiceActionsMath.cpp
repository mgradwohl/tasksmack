/// @file test_WindowsServiceActionsMath.cpp
/// @brief WindowsServiceActionsMath.h (#1577): error codes to messages, the dependents message, and
/// start types to SCM codes. No Windows header, so these run on every platform.

#include "Platform/IServiceProbe.h"
#include "Platform/Windows/WindowsServiceActionsMath.h"

#include <gtest/gtest.h>

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

} // namespace
} // namespace Platform::Windows::ServiceActionMath
