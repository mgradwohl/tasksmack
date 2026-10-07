/// @file test_BundledThemes.cpp
/// @brief Every bundled theme keeps the selected row and the selected tab visible (#1190).
///
/// The thresholds are the ones #1190 set: practical floors for selection fills against their
/// neighbours, and WCAG floors for text (4.5:1) and the overline, a non-text indicator (3:1).
/// The ThemePaletteTest suite (#1196) holds every theme to one set of colour roles and hue families.

#include "FallbackTheme.h"
#include "UI/ColorContrast.h"
#include "UI/ColorDifference.h"
#include "UI/Theme.h"
#include "UI/ThemeLoader.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <tuple>
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
        EXPECT_GE(std::abs(lightness(scheme->cpuUser) - lightness(scheme->chartCpuTotal)), CPU_USER_VS_TOTAL_MIN_DL) << name;

        const std::array series{scheme->chartCpuTotal, scheme->cpuUser, scheme->cpuSystem, scheme->cpuIowait};
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
            {"charts.cpu_total", scheme->chartCpuTotal},
            {"charts.memory_cached", scheme->chartMemoryCached},
            {"charts.memory_shared", scheme->chartMemoryShared},
            {"charts.memory_virtual", scheme->chartMemoryVirtual},
            {"charts.swap", scheme->chartSwap},
            {"charts.power", scheme->chartPower},
            {"charts.battery", scheme->chartBattery},
            {"charts.threads", scheme->chartThreads},
            {"charts.handles", scheme->chartHandles},
            {"charts.page_faults", scheme->chartPageFaults},
            {"charts.gdi", scheme->chartGdi},
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
                    {"charts.cpu_total", scheme->chartCpuTotal},
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
                    {"charts.memory_cached", scheme->chartMemoryCached},
                    {"charts.swap", scheme->chartSwap},
                },
            },
            // Process Details' Memory chart: Used, Shared and Virtual.
            {
                "Process memory",
                {
                    {"charts.memory", scheme->chartMemory},
                    {"charts.memory_shared", scheme->chartMemoryShared},
                    {"charts.memory_virtual", scheme->chartMemoryVirtual},
                },
            },
            // Power & Battery chart (SystemMetricsPanel.cpp).
            {
                "Power",
                {
                    {"charts.power", scheme->chartPower},
                    {"charts.battery", scheme->chartBattery},
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
                    {"charts.threads", scheme->chartThreads},
                    {"charts.handles", scheme->chartHandles},
                    {"charts.page_faults", scheme->chartPageFaults},
                    {"charts.gdi", scheme->chartGdi},
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

// ---- #1196: every metric has its own colour role, and each role keeps one hue family in every theme ----
//
// Thresholds, all CIEDE2000 (about 1 is just noticeable) or OKLCH (hue in degrees, chroma 0..~0.37):
// - STATUS_MIN_DE 10: #1196's acceptance bar. Below about 10 a thin line reads as "the error colour"
//   or "the warning colour" at a glance; an equal hex (disk read == text_error in 13 themes) is 0.
//   Held against success and running too: they are state colours, not series colours.
// - SEVERITY_MIN_DE 20: error and warning against success, #1196's bar for the Monochrome themes,
//   applied everywhere. ERROR_VS_WARNING_MIN_DE 15: the two alarm levels must not merge either.
// - OVERVIEW_MIN_DE 10: series on different charts of the System Overview are different metrics; the
//   same bar as a status colour, so one colour never means two metrics on one screen.
// - Hue ranges are OKLCH, whose hue is even enough to name families by (CIELAB bends blue to violet).
//   They are 30-55 degrees wide: room for each theme's own shade, too narrow to slip into a neighbour.
//   A hue counts only with chroma >= FAMILY_MIN_CHROMA 0.03, about where a tint stops reading as grey;
//   the Monochrome themes' low-chroma tints sit just above it.
// - PAIR_MAX_HUE_DIFF 30: a light variant keeps its dark sibling's families (Nord vs Nord Light).
constexpr double STATUS_MIN_DE = 10.0;
constexpr double SEVERITY_MIN_DE = 20.0;
constexpr double ERROR_VS_WARNING_MIN_DE = 15.0;
constexpr double OVERVIEW_MIN_DE = 10.0;
constexpr double FAMILY_MIN_CHROMA = 0.03;
constexpr double WRITE_CHROMA_BELOW_READ = 0.02; // disk write is brown/olive: duller than read's orange
constexpr double PAIR_MAX_HUE_DIFF = 30.0;

/// An OKLCH hue range in degrees, lo..hi.
struct HueRange
{
    double lo = 0.0;
    double hi = 360.0;
};

// The families (#1196), in hue order: red < orange < amber < yellow < green < teal/cyan < blue < violet
// < magenta. Brown/olive is a dull orange-to-yellow; the load ramp's amber step is a little wider.
constexpr HueRange RED{.lo = 0.0, .hi = 45.0};
constexpr HueRange ORANGE{.lo = 38.0, .hi = 68.0};
constexpr HueRange AMBER{.lo = 62.0, .hi = 92.0};
constexpr HueRange RAMP_AMBER{.lo = 55.0, .hi = 100.0};
constexpr HueRange BROWN_OLIVE{.lo = 68.0, .hi = 118.0};
constexpr HueRange YELLOW{.lo = 88.0, .hi = 118.0};
constexpr HueRange GREEN{.lo = 120.0, .hi = 175.0};
constexpr HueRange CYAN_TEAL{.lo = 175.0, .hi = 228.0};
constexpr HueRange BLUE{.lo = 215.0, .hi = 275.0};
constexpr HueRange VIOLET{.lo = 280.0, .hi = 320.0};
constexpr HueRange MAGENTA{.lo = 318.0, .hi = 360.0};
// Status text: warning is amber to orange (light themes darken it towards brown), success green. Wider
// than the series families: their job is only to read as caution and as fine (Monochrome's were all green).
constexpr HueRange WARNING{.lo = 40.0, .hi = 100.0};
constexpr HueRange SUCCESS{.lo = 115.0, .hi = 175.0};
// No data series is drawn in red: hue 12..36 with chroma >= 0.06. Below 12 is pink, above 36 orange.
constexpr HueRange RED_BAND{.lo = 12.0, .hi = 36.0};
constexpr double RED_BAND_MIN_CHROMA = 0.06;

struct LoadedTheme
{
    std::string name;
    ColorScheme scheme;
};

/// Every bundled theme, and the built-in fallback (Arctic Fire's file, embedded at build time).
auto loadedThemes() -> std::vector<LoadedTheme>
{
    std::vector<LoadedTheme> themes;
    for (const auto& path : bundledThemes())
    {
        if (auto scheme = ThemeLoader::loadTheme(path))
        {
            themes.push_back({.name = path.stem().string(), .scheme = std::move(*scheme)});
        }
        else
        {
            ADD_FAILURE() << "failed to load " << path;
        }
    }
    if (auto fallback = ThemeLoader::loadThemeFromString(FALLBACK_THEME_TOML, "fallback"))
    {
        themes.push_back({.name = "fallback", .scheme = std::move(*fallback)});
    }
    else
    {
        ADD_FAILURE() << "the built-in fallback theme does not parse";
    }
    return themes;
}

using NamedColor = std::pair<std::string_view, ImVec4>;

/// Every colour drawn as a data series: chart lines and fills, NowBars and value-strip swatches.
auto dataSeries(const ColorScheme& s) -> std::vector<NamedColor>
{
    return {
        {"charts.cpu", s.chartCpu},
        {"charts.cpu_total", s.chartCpuTotal},
        {"cpu_breakdown.user", s.cpuUser},
        {"cpu_breakdown.system", s.cpuSystem},
        {"cpu_breakdown.iowait", s.cpuIowait},
        {"charts.memory", s.chartMemory},
        {"charts.memory_cached", s.chartMemoryCached},
        {"charts.memory_shared", s.chartMemoryShared},
        {"charts.memory_virtual", s.chartMemoryVirtual},
        {"charts.swap", s.chartSwap},
        {"charts.io", s.chartIo},
        {"charts.io_write", s.chartIoWrite},
        {"charts.net_tx", s.chartNetTx},
        {"charts.net_rx", s.chartNetRx},
        {"charts.power", s.chartPower},
        {"charts.battery", s.chartBattery},
        {"charts.threads", s.chartThreads},
        {"charts.handles", s.chartHandles},
        {"charts.page_faults", s.chartPageFaults},
        {"charts.gdi", s.chartGdi},
        {"charts.gpu.utilization", s.gpuUtilization},
        {"charts.gpu.memory", s.gpuMemory},
        {"charts.gpu.temperature", s.gpuTemperature},
        {"charts.gpu.power", s.gpuPower},
        {"charts.gpu.encoder", s.gpuEncoder},
        {"charts.gpu.decoder", s.gpuDecoder},
        {"charts.gpu.clock", s.gpuClock},
        {"charts.gpu.fan", s.gpuFan},
    };
}

auto inHueRange(double hue, HueRange range) -> bool
{
    return hue >= range.lo && hue <= range.hi;
}

auto hueDistance(double a, double b) -> double
{
    const double d = std::fmod(std::abs(a - b), 360.0);
    return std::min(d, 360.0 - d);
}

/// "hue 123 chroma 0.045", for failure messages.
auto describeHue(const ImVec4& color) -> std::string
{
    const ColorDifference::Oklch c = ColorDifference::toOklch(color);
    return "hue " + std::to_string(std::lround(c.h)) + " chroma " + std::to_string(c.c);
}

// Status colours are for state and messages. A data series in one reads as an alarm: Swap drew in
// text_error's red in 13 themes, and Temperature in text_warning's hex in 9.
TEST(ThemePaletteTest, StatusColoursAreNeverDataSeries)
{
    for (const auto& [name, s] : loadedThemes())
    {
        const std::array status{
            NamedColor{"semantic.text_error", s.textError},
            NamedColor{"semantic.text_warning", s.textWarning},
            NamedColor{"semantic.text_success", s.textSuccess},
            NamedColor{"status.running", s.statusRunning},
        };
        for (const auto& [seriesKey, series] : dataSeries(s))
        {
            for (const auto& [statusKey, statusColor] : status)
            {
                EXPECT_FALSE(sameRgb(series, statusColor)) << name << ": " << seriesKey << " is " << statusKey;
                EXPECT_GE(ColorDifference::deltaE2000(series, statusColor), STATUS_MIN_DE)
                    << name << ": " << seriesKey << " looks like " << statusKey;
            }
            // Red itself is kept for alarms, whatever the theme's error shade: CPU System, a GPU Decoder or
            // disk read drawn red reads as a problem (pink and orange are fine).
            const ColorDifference::Oklch c = ColorDifference::toOklch(series);
            EXPECT_FALSE(c.c >= RED_BAND_MIN_CHROMA && inHueRange(c.h, RED_BAND))
                << name << ": " << seriesKey << " " << describeHue(series) << " is drawn in alarm red";
        }
    }
}

// A metric is the same family of colour in every theme: CPU blue, memory green, swap violet, disk read
// orange and write brown/olive, network send amber and receive cyan/teal, GPU magenta, power yellow.
TEST(ThemePaletteTest, MetricsKeepTheirHueFamilyInEveryTheme)
{
    for (const auto& [name, s] : loadedThemes())
    {
        const std::array<std::tuple<std::string_view, ImVec4, HueRange>, 15> families{{
            {"charts.cpu", s.chartCpu, BLUE},
            {"charts.cpu_total", s.chartCpuTotal, BLUE},
            {"cpu_breakdown.user", s.cpuUser, BLUE},
            {"charts.memory", s.chartMemory, GREEN},
            {"charts.memory_cached", s.chartMemoryCached, GREEN},
            {"charts.memory_shared", s.chartMemoryShared, GREEN},
            {"charts.gpu.memory", s.gpuMemory, GREEN},
            {"charts.swap", s.chartSwap, VIOLET},
            {"charts.memory_virtual", s.chartMemoryVirtual, VIOLET},
            {"charts.io", s.chartIo, ORANGE},
            {"charts.io_write", s.chartIoWrite, BROWN_OLIVE},
            {"charts.net_tx", s.chartNetTx, AMBER},
            {"charts.net_rx", s.chartNetRx, CYAN_TEAL},
            {"charts.gpu.utilization", s.gpuUtilization, MAGENTA},
            {"charts.power", s.chartPower, YELLOW},
        }};
        for (const auto& [key, color, range] : families)
        {
            const ColorDifference::Oklch c = ColorDifference::toOklch(color);
            EXPECT_GE(c.c, FAMILY_MIN_CHROMA) << name << ": " << key << " is grey";
            EXPECT_TRUE(inHueRange(c.h, range))
                << name << ": " << key << " " << describeHue(color) << " is outside " << range.lo << ".." << range.hi;
        }
        // Read and write can share a warm hue; write is the duller of the two, so it never reads as read.
        EXPECT_LE(ColorDifference::toOklch(s.chartIoWrite).c, ColorDifference::toOklch(s.chartIo).c - WRITE_CHROMA_BELOW_READ)
            << name << ": charts.io_write is as vivid as charts.io";
    }
}

// Power is one colour on every screen: system Power was charts.cpu, process Power text_info and GPU
// Power a field of its own.
TEST(ThemePaletteTest, PowerIsOneColourOnEveryScreen)
{
    for (const auto& [name, s] : loadedThemes())
    {
        EXPECT_TRUE(sameRgb(s.chartPower, s.gpuPower)) << name;
    }
}

// Load bars run green -> amber -> red, and the error colour stays the most alarming: not the palest
// (Monochrome's error was its palest green), and far from warning and success.
TEST(ThemePaletteTest, LoadRampRunsGreenAmberRedAndErrorReadsAsSevere)
{
    for (const auto& [name, s] : loadedThemes())
    {
        const ColorDifference::Oklch low = ColorDifference::toOklch(s.progressLow);
        const ColorDifference::Oklch medium = ColorDifference::toOklch(s.progressMedium);
        const ColorDifference::Oklch high = ColorDifference::toOklch(s.progressHigh);
        const ColorDifference::Oklch error = ColorDifference::toOklch(s.textError);
        const std::array<std::tuple<std::string_view, ColorDifference::Oklch, HueRange>, 6> steps{{
            {"progress.low", low, GREEN},
            {"progress.medium", medium, RAMP_AMBER},
            {"progress.high", high, RED},
            {"semantic.text_error", error, RED},
            {"semantic.text_warning", ColorDifference::toOklch(s.textWarning), WARNING},
            {"semantic.text_success", ColorDifference::toOklch(s.textSuccess), SUCCESS},
        }};
        for (const auto& [key, c, range] : steps)
        {
            EXPECT_GE(c.c, FAMILY_MIN_CHROMA) << name << ": " << key << " is grey";
            EXPECT_TRUE(inHueRange(c.h, range)) << name << ": " << key << " hue " << c.h << " is outside " << range.lo << ".." << range.hi;
        }
        EXPECT_LT(high.h, medium.h) << name << ": the ramp is not ordered red < amber";
        EXPECT_LT(medium.h, low.h) << name << ": the ramp is not ordered amber < green";

        EXPECT_GE(ColorDifference::deltaE2000(s.textError, s.textWarning), ERROR_VS_WARNING_MIN_DE) << name;
        EXPECT_GE(ColorDifference::deltaE2000(s.textError, s.textSuccess), SEVERITY_MIN_DE) << name;
        EXPECT_GE(ColorDifference::deltaE2000(s.textWarning, s.textSuccess), SEVERITY_MIN_DE) << name;

        // The palest colour is the one with the least chroma.
        const double warningChroma = ColorDifference::toOklch(s.textWarning).c;
        const double successChroma = ColorDifference::toOklch(s.textSuccess).c;
        EXPECT_GE(error.c, std::min(warningChroma, successChroma)) << name << ": text_error is the palest status colour";
        EXPECT_GE(high.c, std::min(medium.c, low.c)) << name << ": progress.high is the palest step of the ramp";
    }
}

// On one Tokyo Night Overview screen blue meant CPU Total, CPU User and Cached, and green Memory Used,
// Battery and Handles. Series on different Overview charts are different metrics, so they look different.
TEST(ThemePaletteTest, OverviewChartsGiveEachMetricItsOwnColour)
{
    for (const auto& [name, s] : loadedThemes())
    {
        // {chart, key, colour}. Pairs on one chart are SeriesOnTheSameChartAreSeparable's.
        const std::array<std::tuple<std::string_view, std::string_view, ImVec4>, 12> overview{{
            {"CPU", "charts.cpu_total", s.chartCpuTotal},
            {"CPU", "cpu_breakdown.user", s.cpuUser},
            {"CPU", "cpu_breakdown.system", s.cpuSystem},
            {"CPU", "cpu_breakdown.iowait", s.cpuIowait},
            {"Memory", "charts.memory", s.chartMemory},
            {"Memory", "charts.memory_cached", s.chartMemoryCached},
            {"Memory", "charts.swap", s.chartSwap},
            {"Power", "charts.power", s.chartPower},
            {"Power", "charts.battery", s.chartBattery},
            {"Resources", "charts.threads", s.chartThreads},
            {"Resources", "charts.page_faults", s.chartPageFaults},
            {"Resources", "charts.handles", s.chartHandles},
        }};
        for (std::size_t i = 0; i < overview.size(); ++i)
        {
            for (std::size_t j = i + 1; j < overview.size(); ++j)
            {
                const auto& [chartA, keyA, colorA] = overview[i];
                const auto& [chartB, keyB, colorB] = overview[j];
                if (chartA == chartB)
                {
                    continue;
                }
                EXPECT_GE(ColorDifference::deltaE2000(colorA, colorB), OVERVIEW_MIN_DE) << name << ": " << keyA << " vs " << keyB;
            }
        }
    }
}

// A light variant keeps its dark sibling's families: Nord drew CPU cyan and Nord Light blue.
TEST(ThemePaletteTest, LightAndDarkVariantsShareHueFamilies)
{
    constexpr std::array<std::pair<std::string_view, std::string_view>, 9> PAIRS{{
        {"arctic-fire", "arctic-fire-light"},
        {"cyberpunk", "cyberpunk-light"},
        {"gruvbox", "gruvbox-light"},
        {"mocha", "latte"},
        {"monochrome", "monochrome-light"},
        {"nord", "nord-light"},
        {"solarized-dark", "solarized-light"},
        {"ubuntu-dark", "ubuntu-light"},
        {"windows-dark", "windows-light"},
    }};
    const auto themes = loadedThemes();
    const auto find = [&themes](std::string_view name) -> const ColorScheme*
    {
        const auto it = std::ranges::find(themes, name, &LoadedTheme::name);
        return (it == themes.end()) ? nullptr : &it->scheme;
    };
    for (const auto& [darkName, lightName] : PAIRS)
    {
        const ColorScheme* dark = find(darkName);
        const ColorScheme* light = find(lightName);
        if (dark == nullptr || light == nullptr)
        {
            ADD_FAILURE() << "missing " << darkName << " or " << lightName;
            continue;
        }
        const auto darkSeries = dataSeries(*dark);
        const auto lightSeries = dataSeries(*light);
        for (std::size_t i = 0; i < darkSeries.size(); ++i)
        {
            const ColorDifference::Oklch d = ColorDifference::toOklch(darkSeries[i].second);
            const ColorDifference::Oklch l = ColorDifference::toOklch(lightSeries[i].second);
            if (d.c < FAMILY_MIN_CHROMA || l.c < FAMILY_MIN_CHROMA)
            {
                continue; // A grey (GPU Fan, Handles) has no hue to keep
            }
            EXPECT_LE(hueDistance(d.h, l.h), PAIR_MAX_HUE_DIFF) << darkName << " / " << lightName << ": " << darkSeries[i].first;
        }
    }
}

// The fallback is Arctic Fire: it had 8 identical accents and drew network in the CPU and memory colours.
TEST(ThemePaletteTest, FallbackThemeIsArcticFire)
{
    const auto fallback = ThemeLoader::loadThemeFromString(FALLBACK_THEME_TOML, "fallback");
    const auto arcticFire = ThemeLoader::loadTheme(std::filesystem::path(TASKSMACK_SOURCE_THEMES_DIR) / "arctic-fire.toml");
    ASSERT_TRUE(fallback.has_value());
    ASSERT_TRUE(arcticFire.has_value());
    const auto fallbackSeries = dataSeries(*fallback);
    const auto arcticSeries = dataSeries(*arcticFire);
    for (std::size_t i = 0; i < fallbackSeries.size(); ++i)
    {
        EXPECT_TRUE(sameRgb(fallbackSeries[i].second, arcticSeries[i].second)) << fallbackSeries[i].first;
    }
    for (std::size_t i = 0; i < fallback->accents.size(); ++i)
    {
        EXPECT_TRUE(sameRgb(fallback->accents[i], arcticFire->accents[i])) << "accents[" << i << "]";
    }
    EXPECT_TRUE(sameRgb(fallback->windowBg, arcticFire->windowBg));
    EXPECT_TRUE(sameRgb(fallback->textError, arcticFire->textError));
    EXPECT_TRUE(sameRgb(fallback->progressHigh, arcticFire->progressHigh));
}
} // namespace
} // namespace UI
