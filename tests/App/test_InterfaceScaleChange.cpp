/// @file test_InterfaceScaleChange.cpp
/// @brief Tests for App::changeInterfaceScale() (#3376, #3374)

#include "App/InterfaceScaleChange.h"
#include "App/UserConfig.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>

namespace App
{
namespace
{

/// Restores the saved Interface scale the test changes, and the theme's, so other tests see Auto.
class InterfaceScaleChangeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Original = UserConfig::get().settings().interfaceScalePercent;
        m_OriginalTheme = UI::Theme::get().interfaceScalePercent();
    }

    void TearDown() override
    {
        UserConfig::get().settings().interfaceScalePercent = m_Original;
        UI::Theme::get().setInterfaceScalePercent(m_OriginalTheme);
    }

  private:
    int m_Original = 0;
    int m_OriginalTheme = 0;
};

// The setting and the theme both take the new scale, and Auto (0) goes back to the display's own.
TEST_F(InterfaceScaleChangeTest, SavesTheScaleAndHandsItToTheTheme)
{
    changeInterfaceScale(150);
    EXPECT_EQ(UserConfig::get().settings().interfaceScalePercent, 150);
    EXPECT_EQ(UI::Theme::get().interfaceScalePercent(), 150);

    changeInterfaceScale(0);
    EXPECT_EQ(UserConfig::get().settings().interfaceScalePercent, 0);
    EXPECT_EQ(UI::Theme::get().interfaceScalePercent(), 0);
}

// When only one of them is out of step (the theme was reset, say), the change still reaches both.
TEST_F(InterfaceScaleChangeTest, BringsTheThemeBackInStepWhenOnlyTheSettingAlreadyHasTheScale)
{
    UserConfig::get().settings().interfaceScalePercent = 200;
    UI::Theme::get().setInterfaceScalePercent(0);

    changeInterfaceScale(200);
    EXPECT_EQ(UserConfig::get().settings().interfaceScalePercent, 200);
    EXPECT_EQ(UI::Theme::get().interfaceScalePercent(), 200);

    // Both already at it: nothing changes.
    changeInterfaceScale(200);
    EXPECT_EQ(UserConfig::get().settings().interfaceScalePercent, 200);
    EXPECT_EQ(UI::Theme::get().interfaceScalePercent(), 200);
}

} // namespace
} // namespace App
