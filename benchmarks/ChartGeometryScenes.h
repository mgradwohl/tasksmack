/// @file ChartGeometryScenes.h
/// @brief Headless ImGui + ImPlot scenes that draw the real UI/ChartWidgets.h charts with fixed data,
/// shared by the geometry benchmark (bench_ChartGeometry.cpp) and the vertex/index budget test
/// (tests/UI/test_ChartGeometryBudget.cpp) so both measure exactly the same frames (#1421).
///
/// No window, no renderer and no OpenGL: a context with a display size and a renderer that claims
/// dynamic textures (ImGuiBackendFlags_RendererHasTextures) runs whole frames, NewFrame() through
/// Render(), and leaves the frame's geometry in ImGui::GetDrawData() -- the same precedent as
/// tests/UI/test_FillPlotLayout.cpp and tests/App/test_ProcessTableSettingsRoundTrip.cpp.
///
/// Each scene draws its charts the way the app's panels do (SystemMetricsPanel's stacked CPU chart,
/// CpuCoresSection's per-core grid, MemorySection's memory chart), through the same ChartWidgets
/// calls, so a change to a chart's geometry in ChartWidgets.h shows up here. The data is synthetic and
/// fixed (no RNG distribution, whose output is implementation-defined), the display size and font are
/// fixed, and every scene is warmed up for a few frames before it is measured, so ImPlot's layout
/// (axis label widths from the previous frame) has settled.
///
/// The one input that is not fixed is "now": ChartWidgets anchors its min/max reduction buckets and
/// series markers in absolute time (historyFrameNowSeconds(), steady_clock), so which samples a
/// bucket keeps -- and so the counts -- can move by a few points between runs. The budget test's
/// margin absorbs that.

#pragma once

#include "UI/ChartWidgets.h"
#include "UI/Format.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ChartGeometry
{

/// The fastest refresh interval (SamplingConfig's REFRESH_INTERVAL_MIN_MS), in seconds.
inline constexpr double SAMPLE_INTERVAL_SECONDS = 0.1;
/// 30 minutes at 100 ms (HISTORY_SECONDS_MAX / REFRESH_INTERVAL_MIN_MS): the longest history a chart
/// can show, and what the stacked CPU chart and the long series draw.
inline constexpr std::size_t FULL_HISTORY_SAMPLES = 18'000;
/// 5 minutes at 100 ms (HISTORY_SECONDS_DEFAULT at REFRESH_INTERVAL_MIN_MS): the per-core and memory
/// charts' history.
inline constexpr std::size_t DEFAULT_WINDOW_SAMPLES = 3'000;
/// Cores in the per-core sparkline grid, and its columns.
inline constexpr std::size_t CORE_COUNT = 16;
inline constexpr int CORE_GRID_COLUMNS = 4;

inline constexpr float DISPLAY_WIDTH = 1920.0F;
inline constexpr float DISPLAY_HEIGHT = 1080.0F;
inline constexpr float CHART_HEIGHT = 180.0F; // HISTORY_PLOT_HEIGHT_DEFAULT
inline constexpr float CORE_CHART_HEIGHT = 90.0F;

/// Frames drawn before a scene is measured, so ImPlot's layout has settled.
inline constexpr int WARMUP_FRAMES = 3;

/// The scenes, in the order they are reported.
enum class Scene : std::uint8_t
{
    CpuStacked,        ///< Overview CPU: User/System/IOWait bands, their edges and the Total line, full history
    PerCoreSparklines, ///< CPU Cores grid: CORE_COUNT small filled charts, default window
    Memory,            ///< Memory: Used (filled), Cached and Swap (markers), and the peak line, default window
    LongSeriesMinMax,  ///< One filled line over full history, min/max-reduced afresh every frame (no cache)
};

inline constexpr std::array ALL_SCENES{Scene::CpuStacked, Scene::PerCoreSparklines, Scene::Memory, Scene::LongSeriesMinMax};

[[nodiscard]] constexpr const char* sceneName(Scene scene) noexcept
{
    switch (scene)
    {
    case Scene::CpuStacked:
        return "CpuStacked";
    case Scene::PerCoreSparklines:
        return "PerCoreSparklines";
    case Scene::Memory:
        return "Memory";
    case Scene::LongSeriesMinMax:
        return "LongSeriesMinMax";
    }
    return "Unknown";
}

/// A deterministic value in [0, 1) for (series, index): splitmix64 of the pair. Used instead of a
/// <random> distribution, whose output differs between standard libraries.
[[nodiscard]] constexpr double noise01(std::uint64_t series, std::uint64_t index) noexcept
{
    std::uint64_t z = (series * 0x9E3779B97F4A7C15ULL) + index + 0x632BE59BD9B4E5ADULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    return static_cast<double>(z >> 11U) * 0x1.0p-53;
}

/// A percent series that wanders like a real load: a slow wave plus per-sample noise, in [0, 100].
[[nodiscard]] inline std::vector<float> percentSeries(std::uint64_t series, std::size_t count, double base, double swing)
{
    std::vector<float> out(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const double wave = std::sin(static_cast<double>(i) * 0.013 + static_cast<double>(series)) * swing;
        const double jitter = (noise01(series, i) - 0.5) * swing;
        out[i] = static_cast<float>(std::clamp(base + wave + jitter, 0.0, 100.0));
    }
    return out;
}

/// A history chart's time axis: seconds before now, ascending, one sample every
/// SAMPLE_INTERVAL_SECONDS, the newest half an interval ago (so it is held out to now, as live data is).
[[nodiscard]] inline std::vector<double> timeAxis(std::size_t count)
{
    std::vector<double> out(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        out[i] = -((static_cast<double>(count - i) - 0.5) * SAMPLE_INTERVAL_SECONDS);
    }
    return out;
}

/// The fixed data every scene draws, built once.
struct SceneData
{
    std::vector<double> fullTime = timeAxis(FULL_HISTORY_SAMPLES);
    std::vector<double> windowTime = timeAxis(DEFAULT_WINDOW_SAMPLES);

    // Stacked CPU, full history
    std::vector<float> cpuUser = percentSeries(1, FULL_HISTORY_SAMPLES, 30.0, 20.0);
    std::vector<float> cpuSystem = percentSeries(2, FULL_HISTORY_SAMPLES, 10.0, 8.0);
    std::vector<float> cpuIowait = percentSeries(3, FULL_HISTORY_SAMPLES, 3.0, 4.0);
    std::vector<float> cpuIdle;
    std::vector<float> cpuTotal;

    // Per-core, default window
    std::vector<std::vector<float>> perCore;
    std::vector<std::string> coreLabels;

    // Memory, default window
    std::vector<float> memUsed = percentSeries(20, DEFAULT_WINDOW_SAMPLES, 55.0, 10.0);
    std::vector<float> memCached = percentSeries(21, DEFAULT_WINDOW_SAMPLES, 25.0, 6.0);
    std::vector<float> swapUsed = percentSeries(22, DEFAULT_WINDOW_SAMPLES, 5.0, 3.0);

    // One long series, full history
    std::vector<float> longSeries = percentSeries(30, FULL_HISTORY_SAMPLES, 50.0, 45.0);

    SceneData()
    {
        cpuIdle.resize(FULL_HISTORY_SAMPLES);
        cpuTotal.resize(FULL_HISTORY_SAMPLES);
        for (std::size_t i = 0; i < FULL_HISTORY_SAMPLES; ++i)
        {
            const float busy = std::min(100.0F, cpuUser[i] + cpuSystem[i]);
            cpuIowait[i] = std::min(cpuIowait[i], 100.0F - busy);
            cpuIdle[i] = 100.0F - busy - cpuIowait[i];
            cpuTotal[i] = busy;
        }
        perCore.reserve(CORE_COUNT);
        coreLabels.reserve(CORE_COUNT);
        for (std::size_t core = 0; core < CORE_COUNT; ++core)
        {
            perCore.push_back(percentSeries(100 + core, DEFAULT_WINDOW_SAMPLES, 20.0 + (3.0 * static_cast<double>(core)), 15.0));
            coreLabels.push_back("CPU " + std::to_string(core));
        }
    }
};

/// What a scene keeps between frames, as the panels keep it in members: the stacked chart's point
/// cache and band buffers, and the data generation its charts name (HistoryChartConfig::dataGeneration).
struct SceneState
{
    std::uint64_t dataGeneration = UI::Widgets::nextChartDataGeneration();
    UI::Widgets::ReducedPointsCache cpuStackReduction;
    UI::Widgets::UserSystemStack cpuStack;
    std::vector<double> cpuIowaitTop;
    std::vector<double> cpuBusyTop;
};

/// The geometry of one rendered frame (ImGui::GetDrawData() after ImGui::Render()).
struct FrameGeometry
{
    int vertices = 0;
    int indices = 0;
    int drawLists = 0;
    int commands = 0;
};

/// One ImGui and one ImPlot context for the life of the object, with no ini file and no backend.
class HeadlessChartContext
{
  public:
    HeadlessChartContext() : m_ImGui(ImGui::CreateContext()), m_ImPlot(ImPlot::CreateContext())
    {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        io.DisplayFramebufferScale = ImVec2(1.0F, 1.0F);
        io.DeltaTime = 1.0F / 60.0F;
        // No renderer: let ImGui build and own the font atlas itself.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    ~HeadlessChartContext()
    {
        ImPlot::DestroyContext(m_ImPlot);
        ImGui::DestroyContext(m_ImGui);
    }

    HeadlessChartContext(const HeadlessChartContext&) = delete;
    HeadlessChartContext& operator=(const HeadlessChartContext&) = delete;
    HeadlessChartContext(HeadlessChartContext&&) = delete;
    HeadlessChartContext& operator=(HeadlessChartContext&&) = delete;

  private:
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

namespace Detail
{

inline constexpr const char* CPU_TOTAL_LABEL = "CPU Total";
inline constexpr const char* CPU_USER_LABEL = "User";
inline constexpr const char* CPU_SYSTEM_LABEL = "System";
inline constexpr const char* CPU_IOWAIT_LABEL = "I/O Wait";

[[nodiscard]] inline UI::Widgets::NowBar
bar(std::string_view label, double percent, const ImVec4& color, ImPlotMarker marker = ImPlotMarker_None)
{
    return UI::Widgets::NowBar{.valueText = UI::Format::formatPercent(percent),
                               .label = label,
                               .tooltipText = {},
                               .value01 = UI::Format::percent01(percent),
                               .color = color,
                               .marker = marker};
}

/// SystemMetricsPanel's Overview CPU chart: User, System and I/O Wait bands reduced together and
/// cached per data generation, an edge along each band, the Total line, and the four now bars with
/// their value strip.
inline void drawCpuStacked(const SceneData& data, SceneState& state)
{
    using namespace UI::Widgets;
    const auto& scheme = UI::Theme::get().scheme();
    const double nowSeconds = historyFrameNowSeconds();
    const std::span<const double> time(data.fullTime);
    const std::span<const float> user(data.cpuUser);
    const std::span<const float> system(data.cpuSystem);
    const std::span<const float> iowait(data.cpuIowait);
    const double xMin = -static_cast<double>(FULL_HISTORY_SAMPLES) * SAMPLE_INTERVAL_SECONDS;

    const auto cpuPlot = [&]
    {
        const HistoryChart chart(
            withDataGeneration(withHeight(percentHistoryConfig("##OverviewCPUHistory", xMin, 0.0), CHART_HEIGHT), state.dataGeneration));
        if (!chart.active())
        {
            return;
        }
        const std::span<const ReducedPoint> points = state.cpuStackReduction.points(
            {.generation = state.dataGeneration, .dataId = 0, .count = time.size(), .maxOut = LINE_PLOT_MAX_POINTS_DENSE},
            [&](std::vector<ReducedPoint>& out)
            { reduceAlignedPoints<float>(time, {user, system, iowait}, LINE_PLOT_MAX_POINTS_DENSE, nowSeconds, out); });

        auto& stack = state.cpuStack;
        buildUserSystemStack<float>(points, time, user, system, stack);
        state.cpuIowaitTop.resize(points.size());
        state.cpuBusyTop.resize(points.size());
        for (std::size_t k = 0; k < points.size(); ++k)
        {
            const auto i = static_cast<std::size_t>(points[k].index);
            state.cpuIowaitTop[k] = 100.0 - static_cast<double>(data.cpuIdle[i]);
            state.cpuBusyTop[k] = state.cpuIowaitTop[k] - static_cast<double>(data.cpuIowait[i]);
        }
        holdLastValuesToNow(
            stack.x, {&stack.base, &stack.userTop, &stack.systemTop, &state.cpuIowaitTop, &state.cpuBusyTop}, maxHoldSecondsForAxis(time));
        const int stackCount = UI::Format::checkedCount(stack.x.size());

        plotShadedBand(CPU_USER_LABEL, stack.x.data(), stack.base.data(), stack.userTop.data(), stackCount, scheme.cpuUserFill);
        plotShadedBand(CPU_SYSTEM_LABEL, stack.x.data(), stack.userTop.data(), stack.systemTop.data(), stackCount, scheme.cpuSystemFill);
        plotShadedBand(
            CPU_IOWAIT_LABEL, stack.x.data(), state.cpuBusyTop.data(), state.cpuIowaitTop.data(), stackCount, scheme.cpuIowaitFill);
        plotStyledLine(
            CPU_USER_LABEL, stack.x.data(), stack.userTop.data(), stackCount, scheme.cpuUser, seriesStyle(SeriesRole::Secondary, 0));
        plotStyledLine(
            CPU_SYSTEM_LABEL, stack.x.data(), stack.systemTop.data(), stackCount, scheme.cpuSystem, seriesStyle(SeriesRole::Secondary, 1));
        plotStyledLine(CPU_IOWAIT_LABEL,
                       stack.x.data(),
                       state.cpuIowaitTop.data(),
                       stackCount,
                       scheme.cpuIowait,
                       seriesStyle(SeriesRole::Secondary, 2));
        plotLineWithFill(CPU_TOTAL_LABEL,
                         data.fullTime.data(),
                         data.cpuTotal.data(),
                         UI::Format::checkedCount(data.cpuTotal.size()),
                         scheme.chartCpu,
                         scheme.chartCpuFill,
                         PRIMARY_SERIES_WEIGHT,
                         false);
    };

    NowBarList bars;
    bars.push_back(bar(CPU_TOTAL_LABEL, static_cast<double>(data.cpuTotal.back()), scheme.chartCpu));
    bars.push_back(bar(CPU_USER_LABEL, static_cast<double>(data.cpuUser.back()), scheme.cpuUser));
    bars.push_back(bar(CPU_SYSTEM_LABEL, static_cast<double>(data.cpuSystem.back()), scheme.cpuSystem));
    bars.push_back(bar(CPU_IOWAIT_LABEL, static_cast<double>(data.cpuIowait.back()), scheme.cpuIowait));
    renderHistoryWithNowBars("OverviewCPUHistoryLayout", CHART_HEIGHT, cpuPlot, bars);
}

/// CpuCoresSection's grid: a small filled chart per core with its now bar, no time tick labels.
inline void drawPerCoreSparklines(const SceneData& data, const SceneState& state)
{
    using namespace UI::Widgets;
    const auto& scheme = UI::Theme::get().scheme();
    const double xMin = -static_cast<double>(DEFAULT_WINDOW_SAMPLES) * SAMPLE_INTERVAL_SECONDS;
    if (!ImGui::BeginTable("CoreGrid", CORE_GRID_COLUMNS, ImGuiTableFlags_SizingStretchSame))
    {
        return;
    }
    for (std::size_t core = 0; core < CORE_COUNT; ++core)
    {
        ImGui::TableNextColumn();
        const std::string& coreLabel = data.coreLabels[core];
        const std::vector<float>& samples = data.perCore[core];
        auto coreCfg = percentHistoryConfig(coreLabel.c_str(), xMin, 0.0);
        coreCfg.flags |= ImPlotFlags_NoTitle;
        coreCfg.timeAxisLabels = false;
        coreCfg.height = CORE_CHART_HEIGHT;
        coreCfg.dataGeneration = state.dataGeneration;
        const auto plotFn = [&]
        {
            const HistoryChart chart(coreCfg);
            if (chart.active())
            {
                plotLineWithFill("##Core",
                                 data.windowTime.data(),
                                 samples.data(),
                                 UI::Format::checkedCount(samples.size()),
                                 scheme.chartCpu,
                                 scheme.chartCpuFill,
                                 2.0F,
                                 true,
                                 LINE_PLOT_MAX_POINTS_DENSE);
            }
        };
        renderHistoryWithNowBars(coreLabel.c_str(),
                                 CORE_CHART_HEIGHT,
                                 plotFn,
                                 {bar(coreLabel, static_cast<double>(samples.back()), scheme.chartCpu)},
                                 false,
                                 0,
                                 false,
                                 NowBarValues::None);
    }
    ImGui::EndTable();
}

/// MemorySection's chart: Used (primary, filled), Cached and Swap (secondaries with markers), the
/// window's peak as a reference line, and three now bars with their value strip.
inline void drawMemory(const SceneData& data, const SceneState& state)
{
    using namespace UI::Widgets;
    const auto& scheme = UI::Theme::get().scheme();
    const double xMin = -static_cast<double>(DEFAULT_WINDOW_SAMPLES) * SAMPLE_INTERVAL_SECONDS;
    const int count = UI::Format::checkedCount(data.windowTime.size());
    const double peak = maxOfSeriesSince(std::span<const double>(data.windowTime), xMin, std::span<const float>(data.memUsed));
    const SeriesStyle cachedStyle = seriesStyle(SeriesRole::Secondary, 0);
    const SeriesStyle swapStyle = seriesStyle(SeriesRole::Secondary, 1);

    const auto memoryPlot = [&]
    {
        const HistoryChart chart(
            withDataGeneration(withHeight(percentHistoryConfig("##MemorySwapHistory", xMin, 0.0), CHART_HEIGHT), state.dataGeneration));
        if (!chart.active())
        {
            return;
        }
        plotSeries("Used",
                   data.windowTime.data(),
                   data.memUsed.data(),
                   count,
                   scheme.chartMemory,
                   scheme.chartMemoryFill,
                   seriesStyle(SeriesRole::Primary));
        plotSeries("Cached", data.windowTime.data(), data.memCached.data(), count, scheme.chartCpu, scheme.chartCpuFill, cachedStyle);
        plotSeries("Swap", data.windowTime.data(), data.swapUsed.data(), count, scheme.chartIo, scheme.chartIoFill, swapStyle);
        const std::array<double, 2> xLine = {xMin, 0.0};
        const std::array<double, 2> yLine = {peak, peak};
        ImPlot::PlotLine(
            "Peak", xLine.data(), yLine.data(), 2, {ImPlotProp_LineColor, scheme.chartPeakLine, ImPlotProp_LineWeight, lineWeight(1.5F)});
    };

    renderHistoryWithNowBars("MemorySwapHistoryLayout",
                             CHART_HEIGHT,
                             memoryPlot,
                             {bar("Used", static_cast<double>(data.memUsed.back()), scheme.chartMemory),
                              bar("Cached", static_cast<double>(data.memCached.back()), scheme.chartCpu, cachedStyle.marker),
                              bar("Swap", static_cast<double>(data.swapUsed.back()), scheme.chartIo, swapStyle.marker)});
}

/// One filled line over the full history in a chart that names no data generation, so
/// plotLineWithFill() min/max-reduces all FULL_HISTORY_SAMPLES samples on every frame.
inline void drawLongSeriesMinMax(const SceneData& data)
{
    using namespace UI::Widgets;
    const auto& scheme = UI::Theme::get().scheme();
    const double xMin = -static_cast<double>(FULL_HISTORY_SAMPLES) * SAMPLE_INTERVAL_SECONDS;
    const HistoryChart chart(withHeight(percentHistoryConfig("##LongSeries", xMin, 0.0), CHART_HEIGHT));
    if (chart.active())
    {
        plotLineWithFill("##Long",
                         data.fullTime.data(),
                         data.longSeries.data(),
                         UI::Format::checkedCount(data.longSeries.size()),
                         scheme.chartCpu,
                         scheme.chartCpuFill);
    }
}

} // namespace Detail

/// Builds and renders one frame of @p scene (NewFrame() through Render()) and returns its geometry.
/// Needs a current HeadlessChartContext.
inline FrameGeometry renderFrame(Scene scene, const SceneData& data, SceneState& state)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
    ImGui::Begin("Charts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    switch (scene)
    {
    case Scene::CpuStacked:
        Detail::drawCpuStacked(data, state);
        break;
    case Scene::PerCoreSparklines:
        Detail::drawPerCoreSparklines(data, state);
        break;
    case Scene::Memory:
        Detail::drawMemory(data, state);
        break;
    case Scene::LongSeriesMinMax:
        Detail::drawLongSeriesMinMax(data);
        break;
    }
    ImGui::End();
    ImGui::Render();

    const ImDrawData* drawData = ImGui::GetDrawData();
    FrameGeometry geometry;
    if (drawData == nullptr)
    {
        return geometry;
    }
    geometry.vertices = drawData->TotalVtxCount;
    geometry.indices = drawData->TotalIdxCount;
    geometry.drawLists = drawData->CmdListsCount;
    for (const ImDrawList* list : drawData->CmdLists)
    {
        geometry.commands += list->CmdBuffer.Size;
    }
    return geometry;
}

/// Renders WARMUP_FRAMES frames of @p scene, then one more, and returns that last frame's geometry.
inline FrameGeometry renderSettledFrame(Scene scene, const SceneData& data, SceneState& state)
{
    for (int frame = 0; frame < WARMUP_FRAMES; ++frame)
    {
        static_cast<void>(renderFrame(scene, data, state));
    }
    return renderFrame(scene, data, state);
}

} // namespace ChartGeometry
