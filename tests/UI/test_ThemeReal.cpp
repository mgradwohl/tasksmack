/// @file test_ThemeReal.cpp
/// @brief The real UI::Theme (#1547), in its own test executable (TaskSmackThemeTests): the main one
/// links tests/Mocks/ThemeStub.cpp, which defines the same symbols. Covers theme discovery and loading
/// from the bundled themes, switching by id (deferred to the frame boundary), font-size and display-scale
/// changes, and applyImGuiStyle() against a live headless ImGui/ImPlot context.

#include "UI/ColorContrast.h"
#include "UI/FrameOutline.h"
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
        EXPECT_FALSE(themes[i].name.empty()) << i;
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

// #1923: frames stay borderless by default (buttons, headers, charts); a FrameOutlineScope outlines
// the controls drawn inside it in the scheme's frameBorder, and puts the style back after.
TEST_F(ThemeRealTest, FrameOutlineScopeOutlinesOnlyWhileAlive)
{
    Theme& theme = Theme::get();
    theme.setThemeById("windows-light");
    static_cast<void>(theme.applyPendingStyleChanges());
    theme.applyImGuiStyle();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 windowBorder = style.Colors[ImGuiCol_Border];
    EXPECT_FLOAT_EQ(style.FrameBorderSize, 0.0F);
    {
        const Widgets::FrameOutlineScope outline;
        EXPECT_FLOAT_EQ(style.FrameBorderSize, Widgets::FRAME_OUTLINE_SIZE);
        EXPECT_FLOAT_EQ(style.Colors[ImGuiCol_Border].x, theme.scheme().frameBorder.x);
        EXPECT_FLOAT_EQ(style.Colors[ImGuiCol_Border].w, 1.0F);
        EXPECT_GE(ColorContrast::contrastRatio(style.Colors[ImGuiCol_Border], style.Colors[ImGuiCol_WindowBg]),
                  ColorContrast::FRAME_BORDER_CONTRAST_MIN);
    }
    EXPECT_FLOAT_EQ(style.FrameBorderSize, 0.0F);
    EXPECT_FLOAT_EQ(style.Colors[ImGuiCol_Border].x, windowBorder.x);
    EXPECT_FLOAT_EQ(style.Colors[ImGuiCol_Border].w, windowBorder.w);
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

TEST_F(ThemeRealTest, TheFontSizeStepsThroughEveryPresetAndStopsAtTheEnds)
{
    Theme& theme = Theme::get();
    const FontSize original = theme.currentFontSize();
    theme.setFontSize(FontSize::Small);
    // Ctrl+= walks up one preset at a time, then has nowhere to go.
    for (const FontSize expected : {FontSize::Medium, FontSize::Large, FontSize::ExtraLarge, FontSize::Huge, FontSize::EvenHuger})
    {
        EXPECT_TRUE(theme.increaseFontSize());
        EXPECT_EQ(theme.currentFontSize(), expected);
    }
    EXPECT_FALSE(theme.increaseFontSize());
    EXPECT_EQ(theme.currentFontSize(), FontSize::EvenHuger);
    // Ctrl+- walks back down, then stops at Small.
    for (const FontSize expected : {FontSize::Huge, FontSize::ExtraLarge, FontSize::Large, FontSize::Medium, FontSize::Small})
    {
        EXPECT_TRUE(theme.decreaseFontSize());
        EXPECT_EQ(theme.currentFontSize(), expected);
    }
    EXPECT_FALSE(theme.decreaseFontSize());
    EXPECT_EQ(theme.currentFontSize(), FontSize::Small);
    theme.setFontSize(original);
}

TEST_F(ThemeRealTest, AnOutOfRangeThemeIndexChangesNothing)
{
    Theme& theme = Theme::get();
    const std::string before = theme.currentThemeId();
    const std::size_t count = theme.discoveredThemes().size();
    theme.setTheme(count + 5);
    static_cast<void>(theme.applyPendingStyleChanges());
    EXPECT_EQ(theme.currentThemeId(), before);
}

TEST_F(ThemeRealTest, AccentColorsWrapAround)
{
    const Theme& theme = Theme::get();
    for (std::size_t i = 0; i < Theme::accentCount(); ++i)
    {
        const ImVec4 first = theme.accentColor(i);
        const ImVec4 wrapped = theme.accentColor(i + Theme::accentCount());
        EXPECT_FLOAT_EQ(first.x, wrapped.x) << i;
        EXPECT_FLOAT_EQ(first.y, wrapped.y) << i;
        EXPECT_FLOAT_EQ(first.z, wrapped.z) << i;
        EXPECT_FLOAT_EQ(first.w, wrapped.w) << i;
    }
}

TEST_F(ThemeRealTest, ClearingTheFontRegistrationsForgetsThemAndBumpsTheGeneration)
{
    Theme& theme = Theme::get();
    ImFont* font = ImGui::GetIO().Fonts->AddFontDefault();
    for (const FontSize size :
         {FontSize::Small, FontSize::Medium, FontSize::Large, FontSize::ExtraLarge, FontSize::Huge, FontSize::EvenHuger})
    {
        theme.registerFonts(size, font, font);
    }
    theme.registerTitleFont(font);
    EXPECT_EQ(theme.chartFont(), font);
    const auto generation = theme.fontGeneration();

    // UILayer's onDetach(): the atlas the fonts lived in is about to go, so nothing may keep them.
    theme.clearFontRegistrations();
    EXPECT_EQ(theme.chartFont(), nullptr);
    EXPECT_GT(theme.fontGeneration(), generation); // font-dependent caches rebuild
}

// #2338: the headless UI training driver pushed the body font with PushFont(font, 0.0F), which keeps
// the size current at the start of the frame -- the atlas's first font, Small -- so a Medium run drew
// its text at the Small size. UILayer and the driver both go through pushRegularFont(), which must
// select the configured preset's own size.
TEST_F(ThemeRealTest, PushRegularFontSelectsTheConfiguredPresetsSizeNotTheFramesStartingSize)
{
    constexpr float SMALL_PX = 10.0F;
    constexpr float MEDIUM_PX = 20.0F;
    Theme& theme = Theme::get();
    const FontSize original = theme.currentFontSize();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(800.0F, 600.0F);
    io.DeltaTime = 1.0F / 60.0F;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // no renderer: ImGui owns the atlas
    // Registered in the same order as UI::loadAllFonts(): Small first, so it is the frame's default.
    ImFontConfig smallConfig;
    smallConfig.SizePixels = SMALL_PX;
    ImFont* small = io.Fonts->AddFontDefault(&smallConfig);
    ImFontConfig mediumConfig;
    mediumConfig.SizePixels = MEDIUM_PX;
    ImFont* medium = io.Fonts->AddFontDefault(&mediumConfig);
    theme.registerFonts(FontSize::Small, small, small);
    theme.registerFonts(FontSize::Medium, medium, medium);
    theme.setFontSize(FontSize::Medium);
    static_cast<void>(theme.applyPendingStyleChanges());

    ImGui::NewFrame();
    EXPECT_FLOAT_EQ(ImGui::GetFontSize(), SMALL_PX); // what PushFont(font, 0.0F) would have kept
    const ImFont* pushed = theme.pushRegularFont();
    EXPECT_EQ(pushed, medium);
    EXPECT_EQ(ImGui::GetFont(), medium);
    EXPECT_FLOAT_EQ(ImGui::GetFontSize(), MEDIUM_PX);
    EXPECT_FLOAT_EQ(ImGui::GetStyle().FontSizeBase, medium->LegacySize);
    if (pushed != nullptr)
    {
        ImGui::PopFont();
    }
    ImGui::EndFrame();

    // No font registered for the preset: nothing is pushed, and the caller has nothing to pop.
    theme.clearFontRegistrations();
    ImGui::NewFrame();
    const ImFont* fontBefore = ImGui::GetFont();
    const float sizeBefore = ImGui::GetFontSize();
    EXPECT_EQ(theme.pushRegularFont(), nullptr);
    EXPECT_EQ(ImGui::GetFont(), fontBefore);
    EXPECT_FLOAT_EQ(ImGui::GetFontSize(), sizeBefore);
    ImGui::EndFrame();

    theme.setFontSize(original);
    static_cast<void>(theme.applyPendingStyleChanges());
}

} // namespace
} // namespace UI
