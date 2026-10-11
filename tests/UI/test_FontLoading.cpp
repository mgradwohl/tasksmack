/// @file test_FontLoading.cpp
/// @brief The app's fonts (#1547), loaded into a headless ImGui atlas and registered with the real
/// UI::Theme (TaskSmackThemeTests): every Font Size preset from the bundled Inter, bold and Sixtyfour
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
        m_SavedSize = Theme::get().currentFontSize();
    }

    void TearDown() override
    {
        // Theme is a process-wide singleton: leave it at the size it had, with fonts from no atlas.
        Theme::get().setFontSize(m_SavedSize);
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
    FontSize m_SavedSize = FontSize::Medium;
};

TEST_F(FontLoadingTest, LoadsEveryPresetFromTheBundledFonts)
{
    loadAllFonts(files(), assetsDir(), 1.0F);

    Theme& theme = Theme::get();
    for (const FontSize size : ALL_FONT_SIZES)
    {
        theme.setFontSize(size);
        const auto preset = static_cast<int>(size);
        EXPECT_TRUE(namedLike(theme.regularFont(), "Inter-Regular")) << preset;
        EXPECT_TRUE(namedLike(theme.largeFont(), "Inter-Regular")) << preset;
        EXPECT_TRUE(namedLike(theme.boldFont(), "Inter-Bold")) << preset;
        EXPECT_NE(theme.monospaceFont(), nullptr) << preset; // a system face, or ImGui's own
        EXPECT_FLOAT_EQ(theme.regularFont()->LegacySize, computePointsToPixels(theme.fontConfig(size).regularPt, 1.0F)) << preset;
        EXPECT_FLOAT_EQ(theme.largeFont()->LegacySize, computePointsToPixels(theme.fontConfig(size).largePt, 1.0F)) << preset;
    }
    EXPECT_TRUE(namedLike(theme.titleFont(), "Sixtyfour"));
    EXPECT_NE(theme.chromeIconFont(), nullptr);
    EXPECT_FLOAT_EQ(theme.titleBarHeightPx(), computeTitleBarHeightPx(1.0F));
    EXPECT_FLOAT_EQ(theme.chromeIconFontSizePx(), computeChromeIconPx(theme.titleBarHeightPx()));
    // Each file read once, however many fonts are made from it (#1170).
    EXPECT_GE(files().fileCount(), 4U);
}

TEST_F(FontLoadingTest, SizesFollowTheDisplayScale)
{
    loadAllFonts(files(), assetsDir(), 2.0F);

    Theme& theme = Theme::get();
    theme.setFontSize(FontSize::Medium);
    EXPECT_FLOAT_EQ(theme.regularFont()->LegacySize, computePointsToPixels(theme.fontConfig(FontSize::Medium).regularPt, 2.0F));
    EXPECT_FLOAT_EQ(theme.regularFont()->LegacySize, 2.0F * computePointsToPixels(theme.fontConfig(FontSize::Medium).regularPt, 1.0F));
    EXPECT_FLOAT_EQ(theme.titleBarHeightPx(), computeTitleBarHeightPx(2.0F));
}

TEST_F(FontLoadingTest, TheBodyFontHasTheIconsAndTabularDigits)
{
    loadAllFonts(files(), assetsDir(), 1.0F);
    Theme& theme = Theme::get();
    theme.setFontSize(FontSize::Medium);
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
    loadAllFonts(files(), nowhere, 1.0F);

    Theme& theme = Theme::get();
    for (const FontSize size : ALL_FONT_SIZES)
    {
        theme.setFontSize(size);
        const auto preset = static_cast<int>(size);
        ASSERT_NE(theme.regularFont(), nullptr) << preset;
        EXPECT_FALSE(namedLike(theme.regularFont(), "Inter")) << preset;
        EXPECT_NE(theme.largeFont(), nullptr) << preset;
        // No bold face: the header falls back to the body font.
        EXPECT_EQ(theme.boldFont(), theme.regularFont()) << preset;
        EXPECT_FALSE(theme.regularFont()->IsGlyphInFont(codepoint(ICON_FA_GEAR))) << preset;
    }
}

TEST_F(FontLoadingTest, TheFallbackFontsMergeTheIconsWhenTheyCanBeRead)
{
    loadFallbackFonts(files(), assetsDir(), 1.0F);
    Theme& theme = Theme::get();
    theme.setFontSize(FontSize::Large);
    ASSERT_NE(theme.regularFont(), nullptr);
    EXPECT_FALSE(namedLike(theme.regularFont(), "Inter"));
    EXPECT_TRUE(theme.regularFont()->IsGlyphInFont(codepoint(ICON_FA_GEAR)));
    EXPECT_TRUE(theme.largeFont()->IsGlyphInFont(codepoint(ICON_FA_GEAR)));
    EXPECT_FLOAT_EQ(theme.titleBarHeightPx(), computeTitleBarHeightPx(1.0F));
}

TEST_F(FontLoadingTest, TheFallbackFontsDrawWithoutTheIconsWhenThereAreNone)
{
    loadFallbackFonts(files(), {}, 1.5F);
    Theme& theme = Theme::get();
    theme.setFontSize(FontSize::Medium);
    ASSERT_NE(theme.regularFont(), nullptr);
    EXPECT_FALSE(theme.regularFont()->IsGlyphInFont(codepoint(ICON_FA_GEAR)));
    EXPECT_FLOAT_EQ(theme.regularFont()->LegacySize, computePointsToPixels(theme.fontConfig(FontSize::Medium).regularPt, 1.5F));
    EXPECT_FLOAT_EQ(theme.titleBarHeightPx(), computeTitleBarHeightPx(1.5F));
    EXPECT_EQ(files().fileCount(), 0U);
}

} // namespace
} // namespace UI
