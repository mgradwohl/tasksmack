// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "App/ShellLayer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

namespace App
{
namespace
{

// ========== ActiveTab Enum Tests ==========

TEST(ActiveTabTest, EnumValuesAreDistinct)
{
    // Verify each tab has a unique value
    EXPECT_NE(static_cast<std::uint8_t>(ActiveTab::SystemOverview), static_cast<std::uint8_t>(ActiveTab::Processes));
    EXPECT_NE(static_cast<std::uint8_t>(ActiveTab::SystemOverview), static_cast<std::uint8_t>(ActiveTab::ProcessDetails));
    EXPECT_NE(static_cast<std::uint8_t>(ActiveTab::Processes), static_cast<std::uint8_t>(ActiveTab::ProcessDetails));
}

TEST(ActiveTabTest, EnumUnderlyingTypeIsUint8)
{
    // Verify the enum uses the specified underlying type
    static_assert(std::is_same_v<std::underlying_type_t<ActiveTab>, std::uint8_t>);
}

TEST(ActiveTabTest, EnumValuesAreSequential)
{
    // Verify values start at 0 and are sequential (important for array indexing if ever needed)
    EXPECT_EQ(static_cast<std::uint8_t>(ActiveTab::SystemOverview), 0);
    EXPECT_EQ(static_cast<std::uint8_t>(ActiveTab::Processes), 1);
    EXPECT_EQ(static_cast<std::uint8_t>(ActiveTab::ProcessDetails), 2);
}

TEST(ActiveTabTest, CanBeUsedInSwitch)
{
    // Verify enum works correctly in a switch statement
    auto getTabName = [](ActiveTab tab) -> const char*
    {
        switch (tab)
        {
        case ActiveTab::SystemOverview:
            return "System Overview";
        case ActiveTab::Processes:
            return "Processes";
        case ActiveTab::ProcessDetails:
            return "Process Details";
        }
        return "Unknown";
    };

    EXPECT_STREQ(getTabName(ActiveTab::SystemOverview), "System Overview");
    EXPECT_STREQ(getTabName(ActiveTab::Processes), "Processes");
    EXPECT_STREQ(getTabName(ActiveTab::ProcessDetails), "Process Details");
}

TEST(ActiveTabTest, EnumComparison)
{
    const ActiveTab tab1 = ActiveTab::Processes;
    const ActiveTab tab2 = ActiveTab::Processes;
    const ActiveTab tab3 = ActiveTab::SystemOverview;

    EXPECT_EQ(tab1, tab2);
    EXPECT_NE(tab1, tab3);
}

TEST(ActiveTabTest, DefaultValueIsValid)
{
    // When default-initialized (e.g., in a class), the value should be defined
    ActiveTab tab{};
    // Default value will be SystemOverview (0)
    EXPECT_EQ(tab, ActiveTab::SystemOverview);
}

} // namespace
} // namespace App
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
