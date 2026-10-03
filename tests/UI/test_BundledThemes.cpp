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
#include <filesystem>
#include <vector>

namespace UI
{
namespace
{

using ColorContrast::contrastRatio;
using ColorContrast::flattenOver;

constexpr float SELECTED_ROW_MIN = 1.35F; // selected row vs plain and striped rows
constexpr float HOVER_VS_SELECTED_MIN = 1.15F;
constexpr float TEXT_MIN = 4.5F;         // primary text on the selected row
constexpr float SELECTED_TAB_MIN = 1.3F; // selected tab vs unselected tab
constexpr float OVERLINE_MIN = 3.0F;     // overline vs the selected tab

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

        const ImVec4 row = flattenOver(scheme->windowBg, scheme->windowBg);
        const ImVec4 stripe = flattenOver(scheme->tableRowBgAlt, row);
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

} // namespace
} // namespace UI
