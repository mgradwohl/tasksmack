/// @file test_FontLoading.cpp
/// @brief The app's fonts (#1547), loaded into a headless ImGui atlas and registered with the real
/// UI::Theme (TaskSmackThemeTests): the Font size setting (#3375) from the bundled Inter, bold and Sixtyfour
/// faces with Font Awesome merged in, sized from the display scale, with tabular digits (#1201);
/// ImGui's embedded font when the files are missing; and loadFallbackFonts() merging the icons in only
/// when it can read them (#1169).

#include "UI/DpiScale.h"
#include "UI/FontFileCache.h"
#include "UI/FontLoading.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // ImTextCharFromUtf8()

#include <filesystem>
#include <memory>
#include <string_view>

namespace UI
{
namespace
{

/// The repository's assets directory (Inter, Sixtyfour, Font Awesome).
[[nodiscard]] std::filesystem::path assetsDir()
{
    return {TASKSMACK_SOURCE_ASSETS_DIR};
}

/// The code point of a one-glyph UTF-8 string such as an ICON_FA_* macro.
[[nodiscard]] ImWchar codepoint(const char* utf8)
{
    unsigned int c = 0;
    ImTextCharFromUtf8(&c, utf8, nullptr);
    return static_cast<ImWchar>(c);
}

[[nodiscard]] bool namedLike(const ImFont* font, std::string_view name)
{
    return font != nullptr && std::string_view(font->GetDebugName()).contains(name);
}

class FontLoadingTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(800.0F, 600.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // ImGui owns the atlas
        m_SavedSize = Theme::get().fontPointSize();
    }

    void TearDown() override
    {
        // Theme is a process-wide singleton: leave it at the size it had, with fonts from no atlas.
        Theme::get().setFontPointSize(m_SavedSize);
        Theme::get().clearFontRegistrations();
        ImGui::DestroyContext(m_Context);
        m_Files.reset();
    }

    /// The cache must outlive the atlas, so it lives until TearDown().
    FontFileCache& files()
    {
        return *m_Files;
    }

  private:
    ImGuiContext* m_Context = nullptr;
    std::unique_ptr<FontFileCache> m_Files = std::make_unique<FontFileCache>();
    int m_SavedSize = FONT_POINT_SIZE_DEFAULT;
};

// #3375: one set of fonts, at the Font size setting, every size from it.
TEST_F(FontLoadingTest, LoadsTheFontSizeSettingFromTheBundledFonts)
{
    for (const int points : {FONT_POINT_SIZE_MIN, FONT_POINT_SIZE_DEFAULT, 10, 12, FONT_POINT_SIZE_MAX})
    {
        ImGui::GetIO().Fonts->ClearFonts();
        Theme& theme = Theme::get();
        theme.clearFontRegistrations();
        theme.setFontPointSize(points);
        loadAllFonts(files(), assetsDir(), 1.0F);

        const FontPointSizes sizes = fontPointSizesFor(points);
        EXPECT_EQ(theme.fontsBuiltPointSize(), points);
        EXPECT_FALSE(theme.fontsNeedRebuild()) << points;
        EXPECT_TRUE(namedLike(theme.regularFont(), "Inter-Regular")) << points;
        EXPECT_TRUE(namedLike(theme.largeFont(), "Inter-Regular")) << points;
        EXPECT_TRUE(namedLike(theme.boldFont(), "Inter-Bold")) << points;
        EXPECT_TRUE(namedLike(theme.chartFont(), "Inter-Regular")) << points;
        EXPECT_FLOAT_EQ(theme.regularFont()->LegacySize, computePointsToPixels(sizes.regularPt, 1.0F)) << points;
        EXPECT_FLOAT_EQ(theme.largeFont()->LegacySize, computePointsToPixels(sizes.largePt, 1.0F)) << points;
        EXPECT_FLOAT_EQ(theme.boldFont()->LegacySize, computePointsToPixels(sizes.regularPt, 1.0F)) << points;
        EXPECT_FLOAT_EQ(theme.chartFont()->LegacySize, computePointsToPixels(sizes.chartPt, 1.0F)) << points;
        EXPECT_TRUE(theme.chartFont()->IsGlyphInFont(codepoint(ICON_FA_GEAR))) << points;
        // At 7 and 8 pt the charts draw in the body font itself; above, in one of their own.
        const bool ownChartFont = chartFontPointSize(points) != points;
        EXPECT_EQ(theme.chartFont() != theme.regularFont(), ownChartFont) << points;
        EXPECT_TRUE(namedLike(theme.titleFont(), "Sixtyfour")) << points;
        EXPECT_NE(theme.chromeIconFont(), nullptr) << points;
        // Regular, large and bold (the icons merged into them), the chart font when it differs, then the
        // title font and the chrome icons: no face nothing draws. Six monospace faces were baked into every
        // atlas, and every display-scale rebuild, without ever being used (#2160); and every named size's
        // fonts, used or not, until #3375.
        EXPECT_EQ(ImGui::GetIO().Fonts->Fonts.Size, (ownChartFont ? 4 : 3) + 2) << points;
    }
    const Theme& theme = Theme::get();
    EXPECT_FLOAT_EQ(theme.titleBarHeightPx(), computeTitleBarHeightPx(1.0F));
    EXPECT_FLOAT_EQ(theme.chromeIconFontSizePx(), computeChromeIconPx(theme.titleBarHeightPx()));
    // Each file read once, however many fonts are made from it and however many times they are rebuilt (#1170).
    EXPECT_GE(files().fileCount(), 4U);
}

TEST_F(FontLoadingTest, SizesFollowTheDisplayScale)
{
    Theme& theme = Theme::get();
    theme.setFontPointSize(FONT_POINT_SIZE_DEFAULT);
    loadAllFonts(files(), assetsDir(), 2.0F);

    const float regularPt = theme.fontConfig().regularPt;
    EXPECT_FLOAT_EQ(theme.regularFont()->LegacySize, computePointsToPixels(regularPt, 2.0F));
    EXPECT_FLOAT_EQ(theme.regularFont()->LegacySize, 2.0F * computePointsToPixels(regularPt, 1.0F));
    EXPECT_FLOAT_EQ(theme.titleBarHeightPx(), computeTitleBarHeightPx(2.0F));
}

TEST_F(FontLoadingTest, TheBodyFontHasTheIconsAndTabularDigits)
{
    Theme& theme = Theme::get();
    theme.setFontPointSize(FONT_POINT_SIZE_DEFAULT);
    loadAllFonts(files(), assetsDir(), 1.0F);
    ImFont* body = theme.regularFont();
    ASSERT_NE(body, nullptr);
    EXPECT_TRUE(body->IsGlyphInFont(codepoint(ICON_FA_GEAR)));
    EXPECT_TRUE(body->IsGlyphInFont(codepoint(ICON_FA_LIST)));
    EXPECT_FALSE(namedLike(theme.boldFont(), "Inter-Regular"));

    // Every digit as wide as the widest, so a changing number does not jitter (#1201).
    ImGui::NewFrame();
    ImGui::PushFont(body, 0.0F);
    const float zero = ImGui::CalcTextSize("0").x;
    EXPECT_GT(zero, 0.0F);
    for (const char* digit : {"1", "2", "3", "4", "5", "6", "7", "8", "9"})
    {
        EXPECT_FLOAT_EQ(ImGui::CalcTextSize(digit).x, zero) << digit;
    }
    // Only the digits: a narrow letter keeps its own width.
    EXPECT_LT(ImGui::CalcTextSize("i").x, zero);
    ImGui::PopFont();
    ImGui::EndFrame();
}

TEST_F(FontLoadingTest, MissingFontFilesFallBackToTheEmbeddedFont)
{
    const std::filesystem::path nowhere = assetsDir() / "no-such-assets-dir";
    for (const int points : {FONT_POINT_SIZE_DEFAULT, 14})
    {
        ImGui::GetIO().Fonts->ClearFonts();
        Theme& theme = Theme::get();
        theme.clearFontRegistrations();
        theme.setFontPointSize(points);
        loadAllFonts(files(), nowhere, 1.0F);

        ASSERT_NE(theme.regularFont(), nullptr) << points;
        EXPECT_FALSE(namedLike(theme.regularFont(), "Inter")) << points;
        EXPECT_NE(theme.largeFont(), nullptr) << points;
        EXPECT_NE(theme.chartFont(), nullptr) << points;
        // No bold face: the header falls back to the body font.
        EXPECT_EQ(theme.boldFont(), theme.regularFont()) << points;
        EXPECT_FALSE(theme.regularFont()->IsGlyphInFont(codepoint(ICON_FA_GEAR))) << points;
        EXPECT_EQ(theme.fontsBuiltPointSize(), points);
    }
}

TEST_F(FontLoadingTest, TheFallbackFontsMergeTheIconsWhenTheyCanBeRead)
{
    Theme& theme = Theme::get();
    theme.setFontPointSize(10);
    loadFallbackFonts(files(), assetsDir(), 1.0F);
    ASSERT_NE(theme.regularFont(), nullptr);
    EXPECT_EQ(theme.fontsBuiltPointSize(), 10);
    EXPECT_FLOAT_EQ(theme.regularFont()->LegacySize, computePointsToPixels(10.0F, 1.0F));
    EXPECT_FLOAT_EQ(theme.chartFont()->LegacySize, computePointsToPixels(8.0F, 1.0F));
    EXPECT_FALSE(namedLike(theme.regularFont(), "Inter"));
    EXPECT_TRUE(theme.regularFont()->IsGlyphInFont(codepoint(ICON_FA_GEAR)));
    EXPECT_TRUE(theme.largeFont()->IsGlyphInFont(codepoint(ICON_FA_GEAR)));
    EXPECT_FLOAT_EQ(theme.titleBarHeightPx(), computeTitleBarHeightPx(1.0F));
}

TEST_F(FontLoadingTest, TheFallbackFontsDrawWithoutTheIconsWhenThereAreNone)
{
    Theme& theme = Theme::get();
    theme.setFontPointSize(FONT_POINT_SIZE_DEFAULT);
    loadFallbackFonts(files(), {}, 1.5F);
    ASSERT_NE(theme.regularFont(), nullptr);
    EXPECT_FALSE(theme.regularFont()->IsGlyphInFont(codepoint(ICON_FA_GEAR)));
    EXPECT_FLOAT_EQ(theme.regularFont()->LegacySize, computePointsToPixels(theme.fontConfig().regularPt, 1.5F));
    EXPECT_FLOAT_EQ(theme.titleBarHeightPx(), computeTitleBarHeightPx(1.5F));
    EXPECT_EQ(files().fileCount(), 0U);
}

} // namespace
} // namespace UI
