/// @file test_FontSizeChange.cpp
/// @brief Tests for App::changeFontSize() (#1076, #1178, #3375)

#include "App/FontSizeChange.h"
#include "App/UserConfig.h"
#include "UI/FontPointSize.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>

namespace App
{
namespace
{

/// Restores the saved font size the test changes, and the theme's, so other tests see the default.
class FontSizeChangeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Original = UserConfig::get().settings().fontPointSize;
        m_OriginalTheme = UI::Theme::get().fontPointSize();
    }

    void TearDown() override
    {
        UserConfig::get().settings().fontPointSize = m_Original;
        UI::Theme::get().setFontPointSize(m_OriginalTheme);
    }

  private:
    int m_Original = UI::FONT_POINT_SIZE_DEFAULT;
    int m_OriginalTheme = UI::FONT_POINT_SIZE_DEFAULT;
};

// #1178: changing the font size raises no event. It used to raise FontSizeChangedEvent, whose only
// consumer forced a process enumeration for a purely visual change; font-dependent caches rebuild
// themselves from the font and UI::Theme::fontGeneration(). With no Application in this test,
// raising any event would throw (Application::get()).
TEST_F(FontSizeChangeTest, SavesTheSizeWithoutRaisingAnEvent)
{
    UserConfig::get().settings().fontPointSize = UI::FONT_POINT_SIZE_DEFAULT;

    EXPECT_NO_THROW(changeFontSize(12));
    EXPECT_EQ(UserConfig::get().settings().fontPointSize, 12);
    EXPECT_EQ(UI::Theme::get().fontPointSize(), 12);

    EXPECT_NO_THROW(changeFontSize(7));
    EXPECT_EQ(UserConfig::get().settings().fontPointSize, 7);
    EXPECT_EQ(UI::Theme::get().fontPointSize(), 7);
}

// #3375: a size outside the range is saved, and applied, as the nearest end of it.
TEST_F(FontSizeChangeTest, AnOutOfRangeSizeIsClampedBeforeItIsSaved)
{
    changeFontSize(UI::FONT_POINT_SIZE_MAX + 20);
    EXPECT_EQ(UserConfig::get().settings().fontPointSize, UI::FONT_POINT_SIZE_MAX);
    EXPECT_EQ(UI::Theme::get().fontPointSize(), UI::FONT_POINT_SIZE_MAX);

    changeFontSize(-3);
    EXPECT_EQ(UserConfig::get().settings().fontPointSize, UI::FONT_POINT_SIZE_MIN);
    EXPECT_EQ(UI::Theme::get().fontPointSize(), UI::FONT_POINT_SIZE_MIN);
}

} // namespace
} // namespace App
