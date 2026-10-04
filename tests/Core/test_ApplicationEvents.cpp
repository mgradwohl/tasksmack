/// @file test_ApplicationEvents.cpp
/// @brief Tests for the settings-change events' startup flag (#1102)

#include "Core/ApplicationEvents.h"

#include <gtest/gtest.h>

namespace Core
{
namespace
{

// A user's change forces a sample so it shows at once; the configured value ShellLayer delivers at
// startup is applied without one, since the models were just seeded (#1102).
TEST(ApplicationEventsTest, SettingsChangesAreUserChangesByDefault)
{
    EXPECT_FALSE(RefreshRateChangedEvent(500).isInitial());
    EXPECT_FALSE(HistoryDurationChangedEvent(600).isInitial());
}

TEST(ApplicationEventsTest, StartupSettingsAreMarkedInitial)
{
    const RefreshRateChangedEvent refresh(250, /*initial=*/true);
    const HistoryDurationChangedEvent history(1800, /*initial=*/true);
    EXPECT_TRUE(refresh.isInitial());
    EXPECT_EQ(refresh.getIntervalMs(), 250);
    EXPECT_TRUE(history.isInitial());
    EXPECT_EQ(history.getSeconds(), 1800);
}

} // namespace
} // namespace Core
