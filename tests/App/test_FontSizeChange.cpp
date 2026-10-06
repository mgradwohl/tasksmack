/// @file test_FontSizeChange.cpp
/// @brief Tests for App::changeFontSize() (#1076, #1178)

#include "App/FontSizeChange.h"
#include "App/UserConfig.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>

namespace App
{
namespace
{

/// Restores the saved font size the test changes, so other tests see the default.
class FontSizeChangeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Original = UserConfig::get().settings().fontSize;
    }

    void TearDown() override
    {
        UserConfig::get().settings().fontSize = m_Original;
    }

  private:
    UI::FontSize m_Original = UI::FontSize::Medium;
};

// #1178: changing the font size raises no event. It used to raise FontSizeChangedEvent, whose only
// consumer forced a process enumeration for a purely visual change; font-dependent caches rebuild
// themselves from the font and UI::Theme::fontGeneration(). With no Application in this test,
// raising any event would throw (Application::get()).
TEST_F(FontSizeChangeTest, SavesTheSizeWithoutRaisingAnEvent)
{
    UserConfig::get().settings().fontSize = UI::FontSize::Medium;

    EXPECT_NO_THROW(changeFontSize(UI::FontSize::Large));
    EXPECT_EQ(UserConfig::get().settings().fontSize, UI::FontSize::Large);

    EXPECT_NO_THROW(changeFontSize(UI::FontSize::Small));
    EXPECT_EQ(UserConfig::get().settings().fontSize, UI::FontSize::Small);
}

} // namespace
} // namespace App
