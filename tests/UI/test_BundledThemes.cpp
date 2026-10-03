/// @file test_BundledThemes.cpp
/// @brief Every bundled theme keeps the selected row and the selected tab visible (#1190).
///
/// The thresholds are the ones #1190 set: practical floors for selection fills against their
/// neighbours, and WCAG floors for text (4.5:1) and the overline, a non-text indicator (3:1).

#include "UI/ColorContrast.h"
#include "UI/Theme.h"
#include "UI/ThemeLoader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <utility>
#include <vector>

namespace UI
{
namespace
{

using ColorContrast::contrastRatio;
using ColorContrast::flattenOver;
using ColorContrast::relativeLuminance;

constexpr float SELECTED_ROW_MIN = 1.35F; // selected row vs plain and striped rows
constexpr float HOVER_VS_SELECTED_MIN = 1.15F;
constexpr float TEXT_MIN = 4.5F;                  // primary text on the selected row
constexpr float SELECTED_TAB_MIN = 1.3F;          // selected tab vs unselected tab
constexpr float OVERLINE_MIN = 3.0F;              // overline vs the selected tab
constexpr float CPU_USER_VS_TOTAL_MIN_DL = 15.0F; // CIELAB L* between CPU User and CPU Total (#1192)
constexpr float SERIES_MIN = 3.0F;                // a series colour (line, NowBar, legend swatch) on its background

/// CIELAB L* (0..100) of an opaque colour. L* depends only on relative luminance.
auto lightness(const ImVec4& color) -> float
{
    const float y = relativeLuminance(color);
    constexpr float EPSILON = 216.0F / 24389.0F;
    constexpr float KAPPA = 24389.0F / 27.0F;
    return (y > EPSILON) ? (116.0F * std::cbrt(y)) - 16.0F : KAPPA * y;
}

auto sameRgb(const ImVec4& a, const ImVec4& b) -> bool
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

auto bundledThemes() -> std::vector<std::filesystem::path>
{
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(TASKSMACK_SOURCE_THEMES_DIR))
    {
        if (entry.path().extension() == ".toml")
        {
            paths.push_back(entry.path());
        }
    }
    std::ranges::sort(paths);
    return paths;
}

TEST(BundledThemesTest, AllThemesAreFound)
{
    EXPECT_GE(bundledThemes().size(), 20U);
}

TEST(BundledThemesTest, SelectedRowStandsOutFromEveryRowAndFromHover)
{
    for (const auto& path : bundledThemes())
    {
        const auto scheme = ThemeLoader::loadTheme(path);
        if (!scheme.has_value())
        {
            ADD_FAILURE() << "failed to load " << path;
            continue;
        }
        const auto name = path.stem().string();

        // ImGui fills even rows with TableRowBg and odd rows with TableRowBgAlt, each over the window,
        // and draws the selection or hover fill on top of the row.
        const ImVec4 window = flattenOver(scheme->windowBg, scheme->windowBg);
        const ImVec4 row = flattenOver(scheme->tableRowBg, window);
        const ImVec4 stripe = flattenOver(scheme->tableRowBgAlt, window);
        const ImVec4 selected = flattenOver(scheme->header, row);
        const ImVec4 hovered = flattenOver(scheme->headerHovered, row);

        EXPECT_GE(contrastRatio(selected, row), SELECTED_ROW_MIN) << name;
        EXPECT_GE(contrastRatio(selected, stripe), SELECTED_ROW_MIN) << name;
        EXPECT_GE(contrastRatio(hovered, selected), HOVER_VS_SELECTED_MIN) << name;
        EXPECT_LT(contrastRatio(hovered, row), contrastRatio(selected, row)) << name << ": hover outshines selection";
        EXPECT_GE(contrastRatio(scheme->textPrimary, selected), TEXT_MIN) << name;
    }
}

TEST(BundledThemesTest, SelectedTabStandsOutAndCarriesAVisibleOverline)
{
    for (const auto& path : bundledThemes())
    {
        const auto scheme = ThemeLoader::loadTheme(path);
        if (!scheme.has_value())
        {
            ADD_FAILURE() << "failed to load " << path;
            continue;
        }
        const auto name = path.stem().string();

        const ImVec4 window = flattenOver(scheme->windowBg, scheme->windowBg);
        const ImVec4 tab = flattenOver(scheme->tab, window);
        const ImVec4 selected = flattenOver(scheme->tabSelected, window);
        const ImVec4 overline = flattenOver(scheme->tabSelectedOverline, selected);

        EXPECT_GE(contrastRatio(selected, tab), SELECTED_TAB_MIN) << name;
        EXPECT_GE(contrastRatio(overline, selected), OVERLINE_MIN) << name;
        EXPECT_GE(contrastRatio(scheme->textPrimary, selected), TEXT_MIN) << name;
    }
}

TEST(BundledThemesTest, CpuChartSeriesAreDistinct)
{
    for (const auto& path : bundledThemes())
    {
        const auto scheme = ThemeLoader::loadTheme(path);
        if (!scheme.has_value())
        {
            ADD_FAILURE() << "failed to load " << path;
            continue;
        }
        const auto name = path.stem().string();

        // User was the Total line's colour in every theme, so the User NowBar looked like Total's (#1192).
        EXPECT_GE(std::abs(lightness(scheme->cpuUser) - lightness(scheme->chartCpu)), CPU_USER_VS_TOTAL_MIN_DL) << name;

        const std::array series{scheme->chartCpu, scheme->cpuUser, scheme->cpuSystem, scheme->cpuIowait};
        for (std::size_t i = 0; i < series.size(); ++i)
        {
            for (std::size_t j = i + 1; j < series.size(); ++j)
            {
                EXPECT_FALSE(sameRgb(series[i], series[j])) << name << ": CPU series " << i << " and " << j << " share a colour";
            }
        }

        // A band's fill is its line's colour at a lower alpha, so band, line and swatch read as one series.
        EXPECT_TRUE(sameRgb(scheme->cpuUserFill, scheme->cpuUser)) << name;
        EXPECT_TRUE(sameRgb(scheme->cpuSystemFill, scheme->cpuSystem)) << name;
        EXPECT_TRUE(sameRgb(scheme->cpuIowaitFill, scheme->cpuIowait)) << name;
    }
}

TEST(BundledThemesTest, TooltipTextIsReadable)
{
    for (const auto& path : bundledThemes())
    {
        const auto scheme = ThemeLoader::loadTheme(path);
        if (!scheme.has_value())
        {
            ADD_FAILURE() << "failed to load " << path;
            continue;
        }

        // Popups (tooltips included) are drawn opaque, flattened over the modal backdrop as
        // Theme::applyImGuiStyle() does; chart tooltip rows use the primary text colour (#1192).
        const ImVec4 popup = flattenOver(scheme->popupBg, flattenOver(scheme->modalWindowDimBg, scheme->windowBg));
        EXPECT_GE(contrastRatio(scheme->textPrimary, popup), TEXT_MIN) << path.stem().string();
    }
}

TEST(BundledThemesTest, CpuBandColoursAreVisibleOnThePlotAndInTheLegend)
{
    for (const auto& path : bundledThemes())
    {
        const auto scheme = ThemeLoader::loadTheme(path);
        if (!scheme.has_value())
        {
            ADD_FAILURE() << "failed to load " << path;
            continue;
        }
        const auto name = path.stem().string();

        // ImPlot fills the plot area with PlotBg (childBg) over FrameBg, and the legend with
        // LegendBg (popupBg) over the plot. The bands' edge lines and legend swatches use the
        // opaque series colour (#1192).
        const ImVec4 plot = flattenOver(scheme->childBg, flattenOver(scheme->frameBg, scheme->windowBg));
        const ImVec4 legend = flattenOver(scheme->popupBg, plot);
        for (const auto& [label, color] :
             {std::pair{"user", scheme->cpuUser}, std::pair{"system", scheme->cpuSystem}, std::pair{"iowait", scheme->cpuIowait}})
        {
            EXPECT_GE(contrastRatio(color, plot), SERIES_MIN) << name << " cpu_breakdown." << label << " on the plot";
            EXPECT_GE(contrastRatio(color, legend), SERIES_MIN) << name << " cpu_breakdown." << label << " in the legend";
        }
    }
}

} // namespace
} // namespace UI
