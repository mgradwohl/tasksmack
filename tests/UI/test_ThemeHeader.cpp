#include "Mocks/ColorSchemeFields.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <cstddef>
#include <set>
#include <type_traits>

namespace UI
{
namespace
{

TEST(ThemeHeaderTest, FontSizeArrayMatchesEnumCount)
{
    using Underlying = std::underlying_type_t<FontSize>;
    constexpr auto enumCount = static_cast<std::size_t>(static_cast<Underlying>(FontSize::Count));

    EXPECT_EQ(ALL_FONT_SIZES.size(), enumCount);
    EXPECT_EQ(FONT_SIZE_COUNT, enumCount);
}

TEST(ThemeHeaderTest, FontSizeArrayContainsUniqueEntries)
{
    std::set<FontSize> uniqueSizes;
    for (const auto size : ALL_FONT_SIZES)
    {
        uniqueSizes.insert(size);
    }

    EXPECT_EQ(uniqueSizes.size(), ALL_FONT_SIZES.size());
}

// Fonts are rasterised at pt * 96 / 72 px at 100 % display scale.
constexpr float PT_TO_PX = 96.0F / 72.0F;

auto presetFor(FontSize size) -> const FontSizeConfig&
{
    return FONT_SIZE_PRESETS[static_cast<std::size_t>(size)];
}

TEST(ThemeHeaderTest, NoPresetSetsBodyTextBelowNinePixels)
{
    for (const auto& preset : FONT_SIZE_PRESETS)
    {
        EXPECT_GE(preset.regularPt * PT_TO_PX, 9.0F) << preset.name;
        EXPECT_GT(preset.largePt, preset.regularPt) << preset.name;
    }
}

TEST(ThemeHeaderTest, PresetsGrowWithEachStep)
{
    for (std::size_t i = 1; i < FONT_SIZE_PRESETS.size(); ++i)
    {
        EXPECT_GT(FONT_SIZE_PRESETS[i].regularPt, FONT_SIZE_PRESETS[i - 1].regularPt) << FONT_SIZE_PRESETS[i].name;
    }
}

TEST(ThemeHeaderTest, ChartTextIsAtLeastTenPixelsAtMedium)
{
    EXPECT_GE(presetFor(chartFontSize(FontSize::Medium)).regularPt * PT_TO_PX, 10.0F);
}

TEST(ThemeHeaderTest, ChartTextNeverExceedsBodyTextAndScalesWithThePreset)
{
    auto previous = chartFontSize(FontSize::Small);
    EXPECT_EQ(previous, FontSize::Small);
    for (const auto size : ALL_FONT_SIZES)
    {
        const auto chart = chartFontSize(size);
        EXPECT_LE(presetFor(chart).regularPt, presetFor(size).regularPt);
        EXPECT_GE(static_cast<int>(chart), static_cast<int>(previous));
        previous = chart;
    }
    EXPECT_EQ(chartFontSize(FontSize::EvenHuger), FontSize::Huge);
}

TEST(ThemeHeaderTest, HexToImVec4ConvertsExpectedChannels)
{
    const ImVec4 color = hexToImVec4(0xFF8040);
    EXPECT_NEAR(color.x, 1.0F, 1e-6F);
    EXPECT_NEAR(color.y, 128.0F / 255.0F, 1e-6F);
    EXPECT_NEAR(color.z, 64.0F / 255.0F, 1e-6F);
    EXPECT_NEAR(color.w, 1.0F, 1e-6F);
}

TEST(ThemeHeaderTest, AccentCountIsEight)
{
    EXPECT_EQ(Theme::accentCount(), 8U);
}

TEST(ThemeHeaderTest, SingletonStartsAtDefaultThemeAndFontSize)
{
    // Theme.cpp is excluded from the test build (see tests/Mocks/ThemeStub.cpp); the stubbed
    // constructor skips loadThemes()/loadDefaultFallbackTheme(), so get() always reflects the
    // in-class defaults here, not a loaded theme. That's exactly what these getters' own
    // coverage needs: the accessor bodies themselves, independent of theme-loading logic
    // (already covered separately in test_ThemeLoader.cpp).
    const auto& theme = Theme::get();

    EXPECT_EQ(theme.currentThemeIndex(), 0U);
    EXPECT_TRUE(theme.discoveredThemes().empty());
    EXPECT_EQ(theme.currentFontSize(), FontSize::Medium);
}

// The headless chart tests and benchmarks draw with the stub's scheme (tests/Mocks/ThemeStub.cpp): a
// colour left at its zero default is drawn invisibly, so a scene would measure nothing for that series
// (#1472). Every colour field, accents included, must be visible -- a role added later too.
TEST(ThemeHeaderTest, StubSchemeDrawsEveryColour)
{
    UI::ColorScheme scheme = Theme::get().scheme();

    // The walk ends at the last declared field, so it covers every colour.
    const std::size_t count = TestColorScheme::colorCount(scheme);
    const auto* const first =
        reinterpret_cast<const std::byte*>(scheme.accents.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* const last =
        reinterpret_cast<const std::byte*>(&scheme.modalWindowDimBg); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    EXPECT_EQ(static_cast<std::size_t>(last - first), (count - 1) * sizeof(ImVec4));

    std::size_t index = 0;
    TestColorScheme::forEachColor(scheme,
                                  [&index](ImVec4& color)
                                  {
                                      EXPECT_GT(color.w, 0.0F) << "colour #" << index << " is invisible in the stub scheme";
                                      ++index;
                                  });
    EXPECT_EQ(index, count);
}

TEST(ThemeHeaderTest, WithAlphaReturnsColorWithUpdatedAlpha)
{
    const ImVec4 original{0.1F, 0.2F, 0.3F, 0.4F};
    const ImVec4 updated = withAlpha(original, 0.85F);

    EXPECT_NEAR(updated.x, original.x, 1e-6F);
    EXPECT_NEAR(updated.y, original.y, 1e-6F);
    EXPECT_NEAR(updated.z, original.z, 1e-6F);
    EXPECT_NEAR(updated.w, 0.85F, 1e-6F);
}

} // namespace
} // namespace UI
