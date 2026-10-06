/// @file test_BundledThemes.cpp
/// @brief Every bundled theme keeps the selected row and the selected tab visible (#1190).
///
/// The thresholds are the ones #1190 set: practical floors for selection fills against their
/// neighbours, and WCAG floors for text (4.5:1) and the overline, a non-text indicator (3:1).

#include "UI/ColorContrast.h"
#include "UI/ColorDifference.h"
#include "UI/Theme.h"
#include "UI/ThemeLoader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
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
constexpr float PRIMARY_TEXT_MIN = 7.0F;          // primary text on the window and plot (#1167)
constexpr float GRID_MIN = 1.3F;                  // grid lines on the plot: visible...
constexpr float GRID_MAX = 1.8F;                  // ...but quieter than any series (#1191)
constexpr float SERIES_MIN = 3.0F;                // a series colour (line, NowBar, strip swatch) on its background
constexpr double SAME_CHART_MIN_DE = 12.0;        // CIEDE2000 between series drawn together (#1197)...
constexpr double SAME_CHART_MIN_DE_CVD = 8.0;     // ...and as a protanope or deuteranope sees them

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

TEST(BundledThemesTest, CpuBandColoursAreVisibleOnThePlotAndInTheValueStrip)
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

        // ImPlot fills the plot area with PlotBg (childBg) over FrameBg; the value strip, the chart's
        // key (#1198), sits on the window above it. The bands' edge lines and strip swatches use the
        // opaque series colour (#1192).
        const ImVec4 plot = flattenOver(scheme->childBg, flattenOver(scheme->frameBg, scheme->windowBg));
        const ImVec4 strip = scheme->windowBg;
        for (const auto& [label, color] :
             {std::pair{"user", scheme->cpuUser}, std::pair{"system", scheme->cpuSystem}, std::pair{"iowait", scheme->cpuIowait}})
        {
            EXPECT_GE(contrastRatio(color, plot), SERIES_MIN) << name << " cpu_breakdown." << label << " on the plot";
            EXPECT_GE(contrastRatio(color, strip), SERIES_MIN) << name << " cpu_breakdown." << label << " in the value strip";
        }
    }
}

/// The backgrounds a theme's colours are drawn on, composited the way ImGui and ImPlot draw them.
struct Backgrounds
{
    ImVec4 window;
    ImVec4 frame;          ///< ImPlot's frame, and the NowBar track
    ImVec4 plot;           ///< PlotBg (childBg) over the frame
    ImVec4 popup;          ///< popupBg over the modal backdrop, as Theme::applyImGuiStyle() flattens it
    ImVec4 row;            ///< TableRowBg over the window
    ImVec4 stripe;         ///< TableRowBgAlt over the window
    ImVec4 selected;       ///< The selection fill (Header) over a plain row
    ImVec4 selectedStripe; ///< The selection fill over a striped row
};

auto backgroundsOf(const ColorScheme& scheme) -> Backgrounds
{
    Backgrounds b{};
    b.window = flattenOver(scheme.windowBg, scheme.windowBg);
    b.frame = flattenOver(scheme.frameBg, b.window);
    b.plot = flattenOver(scheme.childBg, b.frame);
    b.popup = flattenOver(scheme.popupBg, flattenOver(scheme.modalWindowDimBg, b.window));
    b.row = flattenOver(scheme.tableRowBg, b.window);
    b.stripe = flattenOver(scheme.tableRowBgAlt, b.window);
    b.selected = flattenOver(scheme.header, b.row);
    b.selectedStripe = flattenOver(scheme.header, b.stripe);
    return b;
}

// #1191: every series colour -- lines, NowBars, strip swatches -- is at least 3:1 on the plot and on
// the NowBar track (WCAG 1.4.11). A bar that fades into its track reads as zero.
TEST(BundledThemesTest, EverySeriesIsVisibleOnThePlotAndTheNowBarTrack)
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
        const Backgrounds bg = backgroundsOf(*scheme);

        std::vector<std::pair<std::string, ImVec4>> series{
            {"charts.cpu", scheme->chartCpu},
            {"charts.memory", scheme->chartMemory},
            {"charts.io", scheme->chartIo},
            {"charts.io_write", scheme->chartIoWrite},
            {"charts.net_tx", scheme->chartNetTx},
            {"charts.net_rx", scheme->chartNetRx},
            {"cpu_breakdown.user", scheme->cpuUser},
            {"cpu_breakdown.system", scheme->cpuSystem},
            {"cpu_breakdown.iowait", scheme->cpuIowait},
            {"charts.gpu.utilization", scheme->gpuUtilization},
            {"charts.gpu.memory", scheme->gpuMemory},
            {"charts.gpu.temperature", scheme->gpuTemperature},
            {"charts.gpu.power", scheme->gpuPower},
            {"charts.gpu.encoder", scheme->gpuEncoder},
            {"charts.gpu.decoder", scheme->gpuDecoder},
            {"charts.gpu.clock", scheme->gpuClock},
            {"charts.gpu.fan", scheme->gpuFan},
            {"progress.low", scheme->progressLow},
            {"progress.medium", scheme->progressMedium},
            {"progress.high", scheme->progressHigh},
        };
        for (std::size_t i = 0; i < scheme->accents.size(); ++i)
        {
            series.emplace_back("accents[" + std::to_string(i) + "]", scheme->accents[i]);
        }

        for (const auto& [key, color] : series)
        {
            EXPECT_GE(contrastRatio(color, bg.plot), SERIES_MIN) << name << " " << key << " on the plot";
            EXPECT_GE(contrastRatio(color, bg.frame), SERIES_MIN) << name << " " << key << " on the NowBar track";
        }

        // The peak line is drawn translucent, so it is judged as drawn: composited over each background.
        EXPECT_GE(contrastRatio(flattenOver(scheme->chartPeakLine, bg.plot), bg.plot), SERIES_MIN)
            << name << " charts.peak_line on the plot";
        EXPECT_GE(contrastRatio(flattenOver(scheme->chartPeakLine, bg.frame), bg.frame), SERIES_MIN)
            << name << " charts.peak_line on the NowBar track";
    }
}

// #1191: grid lines can be seen, but stay quieter than the data.
TEST(BundledThemesTest, GridLinesAreVisibleButQuiet)
{
    for (const auto& path : bundledThemes())
    {
        const auto scheme = ThemeLoader::loadTheme(path);
        if (!scheme.has_value())
        {
            ADD_FAILURE() << "failed to load " << path;
            continue;
        }
        const Backgrounds bg = backgroundsOf(*scheme);
        const float ratio = contrastRatio(flattenOver(scheme->plotGrid, bg.plot), bg.plot);
        EXPECT_GE(ratio, GRID_MIN) << path.stem().string();
        EXPECT_LE(ratio, GRID_MAX) << path.stem().string();
    }
}

// #1167: every text role is readable (4.5:1) wherever it is drawn: the window, the plot (axis labels
// and hints use text_muted) and popups; primary text reaches 7:1 on the window and plot.
TEST(BundledThemesTest, TextRolesAreReadableOnEveryBackground)
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
        const Backgrounds bg = backgroundsOf(*scheme);
        EXPECT_GE(contrastRatio(scheme->textPrimary, bg.window), PRIMARY_TEXT_MIN) << name << " text_primary on the window";
        EXPECT_GE(contrastRatio(scheme->textPrimary, bg.plot), PRIMARY_TEXT_MIN) << name << " text_primary on the plot";
        for (const auto& [key, color] : {
                 std::pair{"text_primary", scheme->textPrimary},
                 std::pair{"text_muted", scheme->textMuted},
                 std::pair{"text_error", scheme->textError},
                 std::pair{"text_warning", scheme->textWarning},
                 std::pair{"text_success", scheme->textSuccess},
                 std::pair{"text_info", scheme->textInfo},
             })
        {
            EXPECT_GE(contrastRatio(color, bg.window), TEXT_MIN) << name << " semantic." << key << " on the window";
            EXPECT_GE(contrastRatio(color, bg.plot), TEXT_MIN) << name << " semantic." << key << " on the plot";
            EXPECT_GE(contrastRatio(color, bg.popup), TEXT_MIN) << name << " semantic." << key << " in popups";
        }
    }
}

// #1167: process status letters are readable on plain, striped and selected rows.
TEST(BundledThemesTest, StatusColoursAreReadableOnEveryRow)
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
        const Backgrounds bg = backgroundsOf(*scheme);
        for (const auto& [key, color] : {
                 std::pair{"running", scheme->statusRunning},
                 std::pair{"sleeping", scheme->statusSleeping},
                 std::pair{"disk_sleep", scheme->statusDiskSleep},
                 std::pair{"zombie", scheme->statusZombie},
                 std::pair{"stopped", scheme->statusStopped},
                 std::pair{"idle", scheme->statusIdle},
             })
        {
            EXPECT_GE(contrastRatio(color, bg.row), TEXT_MIN) << name << " status." << key << " on a plain row";
            EXPECT_GE(contrastRatio(color, bg.stripe), TEXT_MIN) << name << " status." << key << " on a striped row";
            EXPECT_GE(contrastRatio(color, bg.selected), TEXT_MIN) << name << " status." << key << " on the selected row";
            EXPECT_GE(contrastRatio(color, bg.selectedStripe), TEXT_MIN)
                << name << " status." << key << " on the selected row when it is striped";
        }
    }
}

// #1197: series drawn together can be told apart, including by viewers with red/green colour vision
// deficiency. Two series sharing a colour (GPU Memory and Decoder were both #EC407A in Arctic Fire)
// read as one line; pairs that differ only along the red-green axis collapse for about 8% of men.
TEST(BundledThemesTest, SeriesOnTheSameChartAreSeparable)
{
    using ColorDifference::Deficiency;
    using ColorDifference::deltaE2000;
    using ColorDifference::deltaE2000As;

    for (const auto& path : bundledThemes())
    {
        const auto scheme = ThemeLoader::loadTheme(path);
        if (!scheme.has_value())
        {
            ADD_FAILURE() << "failed to load " << path;
            continue;
        }
        const auto name = path.stem().string();

        using Series = std::pair<const char*, ImVec4>;
        const std::vector<std::pair<const char*, std::vector<Series>>> groups{
            // System CPU chart: Total line over the User/System/I/O Wait bands.
            {
                "CPU",
                {
                    {"charts.cpu", scheme->chartCpu},
                    {"cpu_breakdown.user", scheme->cpuUser},
                    {"cpu_breakdown.system", scheme->cpuSystem},
                    {"cpu_breakdown.iowait", scheme->cpuIowait},
                },
            },
            // Memory & Swap chart (MemorySection.cpp): Used, Cached and Swap.
            {
                "Memory",
                {
                    {"charts.memory", scheme->chartMemory},
                    {"charts.cpu", scheme->chartCpu},
                    {"charts.io", scheme->chartIo},
                },
            },
            // Read/Write and Sent/Received charts sit side by side on Process Details' Network and I/O tab.
            {
                "Network and I/O",
                {
                    {"charts.io", scheme->chartIo},
                    {"charts.io_write", scheme->chartIoWrite},
                    {"charts.net_tx", scheme->chartNetTx},
                    {"charts.net_rx", scheme->chartNetRx},
                },
            },
            // GPU Core & Video chart (GpuSection.cpp).
            {
                "GPU core",
                {
                    {"charts.gpu.utilization", scheme->gpuUtilization},
                    {"charts.gpu.memory", scheme->gpuMemory},
                    {"charts.gpu.encoder", scheme->gpuEncoder},
                    {"charts.gpu.decoder", scheme->gpuDecoder},
                    {"charts.gpu.clock", scheme->gpuClock},
                },
            },
            // GPU thermal chart.
            {
                "GPU thermal",
                {
                    {"charts.gpu.temperature", scheme->gpuTemperature},
                    {"charts.gpu.power", scheme->gpuPower},
                    {"charts.gpu.fan", scheme->gpuFan},
                },
            },
            // Resources chart (SystemMetricsPanel.cpp and ProcessDetailsPanel.cpp): Threads, Handles (FDs),
            // Page Faults and, on Windows, GDI objects.
            {
                "Resources",
                {
                    {"charts.cpu", scheme->chartCpu},
                    {"charts.memory", scheme->chartMemory},
                    {"accents[3]", scheme->accents[3]},
                    {"accents[4]", scheme->accents[4]},
                },
            },
        };

        // Status messages and usage bars are not drawn as series on one chart, so #1197 asks only that
        // they survive red/green colour vision deficiency: warning and success must not read as one colour.
        const std::vector<std::pair<const char*, std::vector<Series>>> cvdOnlySets{
            {
                "status",
                {
                    {"semantic.text_error", scheme->textError},
                    {"semantic.text_warning", scheme->textWarning},
                    {"semantic.text_success", scheme->textSuccess},
                    {"semantic.text_info", scheme->textInfo},
                },
            },
            {
                "progress",
                {
                    {"progress.low", scheme->progressLow},
                    {"progress.medium", scheme->progressMedium},
                    {"progress.high", scheme->progressHigh},
                },
            },
        };

        const auto checkPairs = [&](const char* group, const std::vector<Series>& series, bool normalVision)
        {
            for (std::size_t i = 0; i < series.size(); ++i)
            {
                for (std::size_t j = i + 1; j < series.size(); ++j)
                {
                    const auto& [keyA, colorA] = series[i];
                    const auto& [keyB, colorB] = series[j];
                    const std::string pair = name + " [" + group + "] " + keyA + " vs " + keyB;
                    if (normalVision)
                    {
                        EXPECT_GE(deltaE2000(colorA, colorB), SAME_CHART_MIN_DE) << pair;
                    }
                    EXPECT_GE(deltaE2000As(colorA, colorB, Deficiency::Protanopia), SAME_CHART_MIN_DE_CVD) << pair << " (protanopia)";
                    EXPECT_GE(deltaE2000As(colorA, colorB, Deficiency::Deuteranopia), SAME_CHART_MIN_DE_CVD) << pair << " (deuteranopia)";
                }
            }
        };
        for (const auto& [group, series] : groups)
        {
            checkPairs(group, series, true);
        }
        for (const auto& [group, series] : cvdOnlySets)
        {
            checkPairs(group, series, false);
        }
    }
}
} // namespace
} // namespace UI
