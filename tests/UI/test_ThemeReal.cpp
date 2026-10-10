/// @file test_ThemeReal.cpp
/// @brief The real UI::Theme (#1547), in its own test executable (TaskSmackThemeTests): the main one
/// links tests/Mocks/ThemeStub.cpp, which defines the same symbols. Covers theme discovery and loading
/// from the bundled themes, switching by id (deferred to the frame boundary), font-size and display-scale
/// changes, and applyImGuiStyle() against a live headless ImGui/ImPlot context.

#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <implot.h>

#include <cstddef>
#include <filesystem>
#include <string>

namespace UI
{
namespace
{

class ThemeRealTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_ImGui = ImGui::CreateContext();
        m_ImPlot = ImPlot::CreateContext();
        Theme::get().loadThemes(std::filesystem::path(TASKSMACK_SOURCE_THEMES_DIR));
    }

    void TearDown() override
    {
        ImPlot::DestroyContext(m_ImPlot);
        ImGui::DestroyContext(m_ImGui);
    }

  private:
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

TEST_F(ThemeRealTest, DiscoversTheBundledThemes)
{
    const auto& themes = Theme::get().discoveredThemes();
    ASSERT_GE(themes.size(), 2U);
    bool foundMocha = false;
    for (std::size_t i = 0; i < themes.size(); ++i)
    {
        EXPECT_FALSE(Theme::get().themeName(i).empty()) << i;
        foundMocha = foundMocha || themes[i].id == "mocha";
    }
    EXPECT_TRUE(foundMocha);
}

TEST_F(ThemeRealTest, SwitchingThemesChangesTheSchemeAtTheFrameBoundary)
{
    Theme& theme = Theme::get();
    theme.setThemeById("mocha");
    static_cast<void>(theme.applyPendingStyleChanges());
    ASSERT_EQ(theme.currentThemeId(), "mocha");
    const ImVec4 mochaBackground = theme.scheme().windowBg;

    theme.setThemeById("latte");
    // Deferred: nothing changes mid-frame until the boundary applies it.
    EXPECT_TRUE(theme.applyPendingStyleChanges());
    EXPECT_EQ(theme.currentThemeId(), "latte");
    const ImVec4 latteBackground = theme.scheme().windowBg;
    // A dark and a light theme differ in their window background.
    EXPECT_NE(mochaBackground.x + mochaBackground.y + mochaBackground.z, latteBackground.x + latteBackground.y + latteBackground.z);

    EXPECT_FALSE(theme.applyPendingStyleChanges()); // nothing left pending
}

TEST_F(ThemeRealTest, ApplyImGuiStyleWritesTheSchemeIntoTheStyle)
{
    Theme& theme = Theme::get();
    theme.setThemeById("mocha");
    static_cast<void>(theme.applyPendingStyleChanges());
    theme.applyImGuiStyle();
    const ImVec4 windowBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    const ImVec4& scheme = theme.scheme().windowBg;
    EXPECT_FLOAT_EQ(windowBg.x, scheme.x);
    EXPECT_FLOAT_EQ(windowBg.y, scheme.y);
    EXPECT_FLOAT_EQ(windowBg.z, scheme.z);
}

TEST_F(ThemeRealTest, FontSizeAndDisplayScaleScaleTheStyle)
{
    Theme& theme = Theme::get();
    theme.setDisplayScale(1.0F);
    theme.setFontSize(FontSize::Medium);
    static_cast<void>(theme.applyPendingStyleChanges());
    const float mediumScale = theme.styleScale();
    const float mediumRegular = theme.fontConfig().regularPt;

    theme.setFontSize(FontSize::Large);
    static_cast<void>(theme.applyPendingStyleChanges());
    EXPECT_EQ(theme.currentFontSize(), FontSize::Large);
    EXPECT_GT(theme.fontConfig().regularPt, mediumRegular);
    EXPECT_GT(theme.styleScale(), mediumScale);

    theme.setDisplayScale(2.0F);
    static_cast<void>(theme.applyPendingStyleChanges());
    EXPECT_FLOAT_EQ(theme.displayScale(), 2.0F);
    theme.setDisplayScale(1.0F);
    theme.setFontSize(FontSize::Medium);
    static_cast<void>(theme.applyPendingStyleChanges());
}

TEST_F(ThemeRealTest, AnUnknownThemeIdKeepsTheCurrentOne)
{
    Theme& theme = Theme::get();
    theme.setThemeById("mocha");
    static_cast<void>(theme.applyPendingStyleChanges());
    theme.setThemeById("no-such-theme");
    static_cast<void>(theme.applyPendingStyleChanges());
    EXPECT_EQ(theme.currentThemeId(), "mocha");
}

} // namespace
} // namespace UI
