/// @file test_ProcessDetailsChartsRender.cpp
/// @brief Process Details' chart tabs rendered headless (#1179, slice 5): each tab draws the plots
/// and series it drew inside ProcessDetailsPanel -- by their ImPlot legend labels -- its empty states
/// draw no plot, the optional series (Shared, Power) follow the capabilities, and a gap-only
/// history and a hovered gap render (the tooltip shows N/A rather than tripping on NaN).

#include "App/Panels/ProcessDetailsChartHelpers.h"
#include "App/Panels/ProcessDetailsCharts.h"
#include "App/Panels/ProcessDetailsHistory.h"
#include "App/Panels/ProcessDetailsPanel_HistoryHelpers.h"
#include "App/Panels/ProcessSmoothedUsage.h"
#include "Domain/ProcessSnapshot.h"
#include "UI/ChartWidgets.h"
#include "UI/FillPlotLayout.h"
#include "UI/RateAxis.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <implot_internal.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{
namespace
{

constexpr float DISPLAY_WIDTH = 1600.0F;
constexpr float DISPLAY_HEIGHT = 1200.0F;
constexpr std::size_t HISTORY_POINTS = 60;
constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();

/// One plot's series, by legend label (series with a hidden "##" label are not listed).
using SeriesLabels = std::set<std::string>;
using Detail::ProcessSeries;

/// What every supported reading of a busy process looks like at sample @p i.
[[nodiscard]] Detail::ProcessHistoryPoint busyPoint(std::size_t i)
{
    const auto v = static_cast<double>(i + 1);
    Detail::ProcessHistoryPoint point;
    point.cpuTotal = v;
    point.cpuUser = v * 0.6;
    point.cpuSystem = v * 0.4;
    point.memoryUsed = v * 1.0e6;
    point.memoryShared = v * 2.0e5;
    point.virtualBytes = v * 4.0e6;
    point.threads = 8.0;
    point.handles = 40.0;
    point.pageFaults = v;
    point.ioRead = v * 1000.0;
    point.ioWrite = v * 500.0;
    point.netSent = v * 300.0;
    point.netReceived = v * 700.0;
    point.power = v * 0.1;
    point.gpuUtil = v;
    point.gpuMemory = v * 1.0e6;
    point.gdiObjects = 20.0;
    point.npuUtil = v * 0.5;
    point.npuDedicatedMemory = 0.0; // An NPU that maps system RAM (Intel AI Boost) has none
    point.npuSharedMemory = v * 2.0e5;
    point.memoryPrivate = v * 4.0e5;
    point.memorySwap = v * 1.0e5;
    return point;
}

/// A point in which every series that can be unread is a gap (NaN).
[[nodiscard]] Detail::ProcessHistoryPoint gapPoint(std::size_t i)
{
    Detail::ProcessHistoryPoint point = busyPoint(i);
    point.handles = NOT_A_NUMBER;
    point.pageFaults = NOT_A_NUMBER;
    point.ioRead = NOT_A_NUMBER;
    point.ioWrite = NOT_A_NUMBER;
    point.netSent = NOT_A_NUMBER;
    point.netReceived = NOT_A_NUMBER;
    point.power = NOT_A_NUMBER;
    point.gpuUtil = NOT_A_NUMBER;
    point.gpuMemory = NOT_A_NUMBER;
    point.gdiObjects = NOT_A_NUMBER;
    point.npuUtil = NOT_A_NUMBER;
    point.npuDedicatedMemory = NOT_A_NUMBER;
    point.npuSharedMemory = NOT_A_NUMBER;
    return point;
}

/// The panel's state the charts read, filled with @p makePoint once a second up to now.
struct ChartInputs
{
    Detail::ProcessDetailsHistory history;
    Detail::ProcessSmoothedUsage smoothed;
    Domain::ProcessSnapshot snapshot;
    ProcessChartContext ctx;

    ~ChartInputs() = default;
    // ctx points at the members above, so a copy would point at the original's.
    ChartInputs(const ChartInputs&) = delete;
    ChartInputs& operator=(const ChartInputs&) = delete;
    ChartInputs(ChartInputs&&) = delete;
    ChartInputs& operator=(ChartInputs&&) = delete;

    explicit ChartInputs(Detail::ProcessHistoryPoint (*makePoint)(std::size_t), bool gpuSupported = true)
    {
        // Timestamps on historyFrameNowSeconds()'s clock, the newest at now.
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        for (std::size_t i = 0; i < HISTORY_POINTS; ++i)
        {
            history.append(now - static_cast<double>(HISTORY_POINTS - 1 - i), makePoint(i), false);
        }
        snapshot.pid = 4242;
        snapshot.name = "charttarget";
        snapshot.memoryBytes = 50'000'000;
        snapshot.peakMemoryBytes = 80'000'000;
        snapshot.gpuMemoryBytes = 1'000'000;
        snapshot.gpuUtilPercent = 12.0;
        const Detail::SampleRateReadings readings{.io = true,
                                                  .network = true,
                                                  .gpuPerProcess = gpuSupported,
                                                  .gpuUtilization = gpuSupported,
                                                  .gpuSupported = gpuSupported,
                                                  .npuPerProcess = gpuSupported,
                                                  .npuUtilization = gpuSupported,
                                                  .npuSupported = gpuSupported};
        smoothed.update(snapshot, readings, 0.0F, std::chrono::milliseconds(1000));
        ctx = ProcessChartContext{
            .history = &history,
            .smoothed = &smoothed,
            .snapshot = &snapshot,
            .historyGeneration = UI::Widgets::nextChartDataGeneration(),
            .maxHistorySeconds = 300.0,
            .peakMemoryBytes = 80'000'000.0,
            .rateReadings = readings,
            .hasSharedMemory = true,
            .hasPowerUsage = false,
        };
    }
};

class ProcessDetailsChartsRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_ImGui = ImGui::CreateContext();
        m_ImPlot = ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        io.DeltaTime = 1.0F / 60.0F;
        // No renderer: let ImGui build and own the font atlas itself.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImPlot::DestroyContext(m_ImPlot);
        ImGui::DestroyContext(m_ImGui);
    }

  public:
    /// runFrame() for the free helpers below.
    static void runFrameForText(const std::function<void()>& body)
    {
        runFrame(body);
    }

  protected:
    /// One frame of a window like the Process Details pane, running @p body inside it.
    static void runFrame(const std::function<void()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }

    /// Every axis of the Overview, Network and I/O, GPU and NPU tabs, for a history of @p makePoint
    /// samples, against the bound its helper's series form gives (EveryAxisCoversTheSeriesItsHelperNames).
    void expectEveryAxisCoversItsSeries(Detail::ProcessHistoryPoint (*makePoint)(std::size_t));

    /// The Overview's charts as the panel draws them: in a fill layout and an aligned chart stack.
    void renderOverview(ProcessDetailsCharts& charts, const ProcessChartContext& ctx)
    {
        runFrame(
            [&]
            {
                UI::Widgets::FillPlotLayout fill(m_OverviewFill);
                const UI::Widgets::AlignedChartStack alignedCharts("##ProcOverviewCharts");
                charts.renderOverviewCharts(ctx, fill);
            });
    }

    /// Every plot ImPlot has seen in this context, each as the set of its series' legend labels.
    [[nodiscard]] static std::vector<SeriesLabels> plotsDrawn()
    {
        std::vector<SeriesLabels> plots;
        ImPlotContext& context = *ImPlot::GetCurrentContext();
        for (int i = 0; i < context.Plots.GetBufSize(); ++i)
        {
            ImPlotPlot* plot = context.Plots.GetByIndex(i);
            if (plot == nullptr)
            {
                continue;
            }
            SeriesLabels labels;
            for (int item = 0; item < plot->Items.GetLegendCount(); ++item)
            {
                labels.emplace(plot->Items.GetLegendLabel(item));
            }
            plots.push_back(std::move(labels));
        }
        return plots;
    }

    /// Hovers the plot whose series are @p labels, near its right edge (or its left one, if
    /// @p atLeftEdge), and renders @p draw two more frames. Returns whether a tooltip window was shown.
    [[nodiscard]] static bool hoverPlot(const SeriesLabels& labels, const std::function<void()>& draw, bool atLeftEdge = false)
    {
        ImPlotContext& context = *ImPlot::GetCurrentContext();
        for (int i = 0; i < context.Plots.GetBufSize(); ++i)
        {
            ImPlotPlot* plot = context.Plots.GetByIndex(i);
            SeriesLabels found;
            for (int item = 0; plot != nullptr && item < plot->Items.GetLegendCount(); ++item)
            {
                found.emplace(plot->Items.GetLegendLabel(item));
            }
            if (plot == nullptr || found != labels)
            {
                continue;
            }
            // Near the right edge: the newest samples, the gap points' too.
            const ImRect rect = plot->PlotRect;
            ImGui::GetIO().AddMousePosEvent(atLeftEdge ? rect.Min.x + 2.0F : rect.Max.x - 2.0F, (rect.Min.y + rect.Max.y) * 0.5F);
            draw();
            draw();
            const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
            return tooltip != nullptr && tooltip->Active;
        }
        ADD_FAILURE() << "no plot with those series";
        return false;
    }

  private:
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
    UI::Widgets::PlotFillState m_OverviewFill;
};

/// The Resources chart's handle count, on its own right-hand axis (#1916).
#ifdef _WIN32
constexpr std::string_view HANDLES_LABEL = "Handles →";
#else
constexpr std::string_view HANDLES_LABEL = "FDs →";
#endif

// The series each Overview chart draws. Functions, not statics: building a std::set can throw.
[[nodiscard]] SeriesLabels resourceSeries()
{
#ifdef _WIN32
    return {"Threads", std::string(HANDLES_LABEL), "Page Faults →", "GDI Objects"};
#else
    return {"Threads", std::string(HANDLES_LABEL), "Page Faults →"};
#endif
}

/// resourceSeries() for gapPoint()'s history. A GDI series with no reading at all is not drawn
/// (#1000), so on Windows it has no GDI Objects entry.
[[nodiscard]] SeriesLabels gapResourceSeries()
{
#ifdef _WIN32
    return {"Threads", "Handles →", "Page Faults →"};
#else
    return resourceSeries();
#endif
}

[[nodiscard]] SeriesLabels cpuSeries()
{
    return {"Total", "User", "System"};
}

[[nodiscard]] SeriesLabels memorySeries()
{
    return {"Peak Memory", "Memory", "Shared", "Virtual →"};
}

TEST_F(ProcessDetailsChartsRenderTest, OverviewDrawsCpuMemoryAndResources)
{
    const ChartInputs inputs(busyPoint);
    ProcessDetailsCharts charts;
    renderOverview(charts, inputs.ctx);
    renderOverview(charts, inputs.ctx);

    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 3U);
    EXPECT_EQ(plots[0], cpuSeries());
    EXPECT_EQ(plots[1], memorySeries());
    EXPECT_EQ(plots[2], resourceSeries());
}

TEST_F(ProcessDetailsChartsRenderTest, OverviewFollowsTheSharedMemoryAndPowerCapabilities)
{
    ChartInputs inputs(busyPoint);
    inputs.ctx.hasSharedMemory = false; // A platform without Shared: no Shared line (#1035)
    inputs.ctx.hasPowerUsage = true;    // Linux with RAPL: a Power chart before Resources
    ProcessDetailsCharts charts;
    renderOverview(charts, inputs.ctx);
    renderOverview(charts, inputs.ctx);

    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 4U);
    EXPECT_EQ(plots[0], cpuSeries());
    EXPECT_EQ(plots[1], (SeriesLabels{"Peak Memory", "Memory", "Virtual →"}));
    EXPECT_EQ(plots[2], (SeriesLabels{"Power"}));
    EXPECT_EQ(plots[3], resourceSeries());
}

// Where the platform reads Private and Swap (Linux, #1885), each is a line in the Memory chart, after
// Shared and before Virtual, with a tooltip row.
TEST_F(ProcessDetailsChartsRenderTest, TheMemoryChartPlotsPrivateAndSwapWhereTheyAreRead)
{
    ChartInputs inputs(busyPoint);
    inputs.ctx.hasPrivateBytes = true;
    inputs.ctx.hasSwapBytes = true;
    ProcessDetailsCharts charts;
    const auto draw = [&]
    {
        renderOverview(charts, inputs.ctx);
    };
    draw();
    draw();

    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 3U);
    const SeriesLabels withPrivateAndSwap{"Peak Memory", "Memory", "Shared", "Private", "Swap", "Virtual →"};
    EXPECT_EQ(plots[1], withPrivateAndSwap);
    EXPECT_TRUE(hoverPlot(withPrivateAndSwap, draw));
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

// The child window drawn for the card @p id (an ImGui child is named "<parent>/<id>_<hash>").
[[nodiscard]] const ImGuiWindow* findCard(const std::string& id)
{
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    for (const ImGuiWindow* window : g.Windows)
    {
        if ((window->Flags & ImGuiWindowFlags_ChildWindow) != 0 && std::string(window->Name).contains("/" + id + "_"))
        {
            return window;
        }
    }
    return nullptr;
}

using CardCharts = std::vector<std::pair<std::string, SeriesLabels>>;

// Each chart section is a bordered card (#1589, #1590), in order, its chart inside it; together the
// cards fill the pane without overflowing it, as the card padding is counted as non-chart height.
void expectChartsInFillingCards(const CardCharts& cards)
{
    float lastBottom = 0.0F;
    ImPlotContext& context = *ImPlot::GetCurrentContext();
    for (const auto& [id, labels] : cards)
    {
        SCOPED_TRACE(id);
        const ImGuiWindow* card = findCard(id);
        ASSERT_NE(card, nullptr);
        EXPECT_NE(card->ChildFlags & ImGuiChildFlags_Borders, 0);
        EXPECT_GE(card->Pos.y, lastBottom); // In order, none overlapping the one before
        lastBottom = card->Pos.y + card->Size.y;

        bool found = false;
        for (int i = 0; i < context.Plots.GetBufSize(); ++i)
        {
            ImPlotPlot* plot = context.Plots.GetByIndex(i);
            SeriesLabels plotLabels;
            for (int item = 0; plot != nullptr && item < plot->Items.GetLegendCount(); ++item)
            {
                plotLabels.emplace(plot->Items.GetLegendLabel(item));
            }
            if (plot == nullptr || plotLabels != labels)
            {
                continue;
            }
            found = true;
            EXPECT_TRUE(card->Rect().Contains(plot->FrameRect)) << "the chart is drawn outside its card";
        }
        EXPECT_TRUE(found);
    }

    const ImGuiWindow* pane = ImGui::FindWindowByName("Details");
    ASSERT_NE(pane, nullptr);
    EXPECT_LE(pane->ScrollMax.y, 0.0F) << "the cards overflow the pane";
    // Filled: what is left below the last card is less than one line, not an empty band.
    EXPECT_LT(pane->InnerRect.Max.y - lastBottom, ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.y);
}

TEST_F(ProcessDetailsChartsRenderTest, OverviewChartsSitInsideCardsThatFillThePane)
{
    ChartInputs inputs(busyPoint);
    inputs.ctx.hasPowerUsage = true;
    ProcessDetailsCharts charts;
    for (int frame = 0; frame < 4; ++frame) // The fill layout measures from the previous frame
    {
        renderOverview(charts, inputs.ctx);
    }

    ASSERT_EQ(plotsDrawn().size(), 4U);
    // Settled, not cycling: an auto-sized card lagged its chart by a frame, and the fill layout's
    // correction for that lag made the heights cycle every six frames without end.
    const ImGuiWindow* cpuCard = findCard("##ProcCpuCard");
    ASSERT_NE(cpuCard, nullptr);
    const float settledHeight = cpuCard->Size.y;
    for (int frame = 0; frame < 12; ++frame)
    {
        renderOverview(charts, inputs.ctx);
        EXPECT_FLOAT_EQ(cpuCard->Size.y, settledHeight) << "frame " << frame;
    }
    expectChartsInFillingCards({
        {"##ProcCpuCard", cpuSeries()},
        {"##ProcMemoryCard", memorySeries()},
        {"##ProcPowerCard", SeriesLabels{"Power"}},
        {"##ProcResourcesCard", resourceSeries()},
    });
}

TEST_F(ProcessDetailsChartsRenderTest, OverviewDrawsNothingWithoutHistory)
{
    ChartInputs inputs(busyPoint);
    const Detail::ProcessDetailsHistory empty;
    inputs.ctx.history = &empty;
    ProcessDetailsCharts charts;
    renderOverview(charts, inputs.ctx);
    EXPECT_TRUE(plotsDrawn().empty());
}

// #1891: a chart card cut off by the bottom of the pane: its header shows but its plot is culled
// (ImPlot's BeginPlot fails), so the chart is skipped. Sliding the charts down a few pixels a frame
// cuts each card in turn, on the Overview (Power included) and the Network and I/O tab; a chart is
// either drawn whole, with the series a full render draws, or not plotted at all.
TEST_F(ProcessDetailsChartsRenderTest, ChartsCutOffByTheBottomOfThePaneAreSkippedWhole)
{
    ChartInputs inputs(busyPoint);
    inputs.ctx.hasPowerUsage = true;
    ProcessDetailsCharts charts;
    UI::Widgets::PlotFillState fillState;
    const auto renderBoth = [&](float offset)
    {
        runFrame(
            [&]
            {
                ImGui::Dummy(ImVec2(1.0F, offset));
                UI::Widgets::FillPlotLayout fill(fillState);
                const UI::Widgets::AlignedChartStack alignedCharts("##ProcOverviewCharts");
                charts.renderOverviewCharts(inputs.ctx, fill);
            });
        runFrame(
            [&]
            {
                ImGui::Dummy(ImVec2(1.0F, offset));
                charts.renderNetworkTab(inputs.ctx);
            });
    };
    renderBoth(1.0F);
    renderBoth(1.0F);
    const auto full = plotsDrawn();
    ASSERT_EQ(full.size(), 6U); // CPU, Memory, Power, Resources; I/O, Network

    for (int step = 0; static_cast<float>(step) * 6.0F < DISPLAY_HEIGHT; ++step)
    {
        const float offset = 1.0F + (static_cast<float>(step) * 6.0F);
        renderBoth(offset);
        for (const SeriesLabels& plot : plotsDrawn())
        {
            EXPECT_TRUE(plot.empty() || std::ranges::find(full, plot) != full.end()) << "at offset " << offset;
        }
    }
}

// #1891: without history the Network and I/O tab draws no chart either, as the Overview doesn't.
TEST_F(ProcessDetailsChartsRenderTest, TheNetworkTabDrawsNothingWithoutHistory)
{
    ChartInputs inputs(busyPoint);
    const Detail::ProcessDetailsHistory empty;
    inputs.ctx.history = &empty;
    ProcessDetailsCharts charts;
    runFrame([&] { charts.renderNetworkTab(inputs.ctx); });
    runFrame([&] { charts.renderNetworkTab(inputs.ctx); });
    EXPECT_TRUE(plotsDrawn().empty());
}

// #1891: the Network card's heading explains its rate on hover: between readings of the process's open
// connections, a cached reading repeating the last rate, and how many samples the chart holds.
TEST_F(ProcessDetailsChartsRenderTest, TheNetworkHeadingExplainsItsRateOnHover)
{
    const ChartInputs inputs(busyPoint);
    ProcessDetailsCharts charts;
    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverStationaryDelay = 0.0F;
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    std::string text;
    const auto frame = [&]
    {
        runFrame(
            [&]
            {
                ImGui::LogToBuffer();
                charts.renderNetworkTab(inputs.ctx);
                text = GImGui->LogBuffer.c_str();
                ImGui::LogFinish();
            });
    };
    frame();
    frame();
    // The heading sits at the card's top-left, left of any plot: sweep down the left margin for it.
    ImGuiIO& io = ImGui::GetIO();
    bool explained = false;
    for (int row = 0; row < static_cast<int>(DISPLAY_HEIGHT) / 4 && !explained; ++row)
    {
        io.AddMousePosEvent(24.0F, static_cast<float>(row * 4));
        frame();
        frame();
        explained = text.contains("between readings of the process's open connections");
    }
    EXPECT_TRUE(explained) << text;
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

TEST_F(ProcessDetailsChartsRenderTest, AMissingInputDrawsNothing)
{
    ChartInputs inputs(busyPoint);
    inputs.ctx.smoothed = nullptr;
    ProcessDetailsCharts charts;
    renderOverview(charts, inputs.ctx);
    runFrame([&] { charts.renderNetworkTab(inputs.ctx); });
    runFrame([&] { charts.renderGpuTab(inputs.ctx); });
    runFrame([&] { charts.renderNpuTab(inputs.ctx); }); // #1891: the NPU tab checks its inputs too
    EXPECT_TRUE(plotsDrawn().empty());
}

TEST_F(ProcessDetailsChartsRenderTest, NetworkTabDrawsIoThenNetwork)
{
    ChartInputs inputs(busyPoint);
    ProcessDetailsCharts charts;
    runFrame([&] { charts.renderNetworkTab(inputs.ctx); });
    runFrame([&] { charts.renderNetworkTab(inputs.ctx); });

    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 2U);
    EXPECT_EQ(plots[0], (SeriesLabels{"Read", "Write"}));
    EXPECT_EQ(plots[1], (SeriesLabels{"Sent", "Received"}));
}

TEST_F(ProcessDetailsChartsRenderTest, NetworkChartsSitInsideCardsThatFillThePane)
{
    ChartInputs inputs(busyPoint);
    ProcessDetailsCharts charts;
    for (int frame = 0; frame < 4; ++frame) // The fill layout measures from the previous frame
    {
        runFrame([&] { charts.renderNetworkTab(inputs.ctx); });
    }

    ASSERT_EQ(plotsDrawn().size(), 2U);
    expectChartsInFillingCards({
        {"##ProcIoCard", SeriesLabels{"Read", "Write"}},
        {"##ProcNetworkCard", SeriesLabels{"Sent", "Received"}},
    });
}

TEST_F(ProcessDetailsChartsRenderTest, NetworkTabWithOnlyGapsShowsItsEmptyState)
{
    ChartInputs inputs(gapPoint);
    ProcessDetailsCharts charts;
    runFrame([&] { charts.renderNetworkTab(inputs.ctx); });
    EXPECT_TRUE(plotsDrawn().empty());
}

TEST_F(ProcessDetailsChartsRenderTest, GpuTabDrawsUtilizationAndMemoryWhenThereIsUsage)
{
    ChartInputs inputs(busyPoint);
    ProcessDetailsCharts charts;
    runFrame([&] { charts.renderGpuTab(inputs.ctx); });
    runFrame([&] { charts.renderGpuTab(inputs.ctx); });

    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 2U);
    EXPECT_EQ(plots[0], (SeriesLabels{"Utilization"}));
    EXPECT_EQ(plots[1], (SeriesLabels{"Memory"}));
}

TEST_F(ProcessDetailsChartsRenderTest, GpuChartsSitInsideCardsBelowTheUsageCard)
{
    // #1594: the GPU tab is cards like the other tabs: the current values in one, each chart in its own.
    ChartInputs inputs(busyPoint);
    ProcessDetailsCharts charts;
    for (int frame = 0; frame < 4; ++frame) // The fill layout measures from the previous frame
    {
        runFrame([&] { charts.renderGpuTab(inputs.ctx); });
        // The auto-sized card's content size is measured from what it drew: never NaN, not even on the
        // first frame, when a table's stretch column has no width yet (UBSan caught 0/0 there).
        if (const ImGuiWindow* card = findCard("##ProcGpuUsageCard"); card != nullptr)
        {
            EXPECT_TRUE(std::isfinite(card->DC.CursorMaxPos.x)) << "frame " << frame;
            EXPECT_TRUE(std::isfinite(card->ContentSize.x)) << "frame " << frame;
        }
    }

    const ImGuiWindow* usage = findCard("##ProcGpuUsageCard");
    ASSERT_NE(usage, nullptr);
    EXPECT_NE(usage->ChildFlags & ImGuiChildFlags_Borders, 0);
    const ImGuiWindow* utilization = findCard("##ProcGpuUtilCard");
    ASSERT_NE(utilization, nullptr);
    EXPECT_LE(usage->Pos.y + usage->Size.y, utilization->Pos.y); // The values above the charts
    expectChartsInFillingCards({
        {"##ProcGpuUtilCard", SeriesLabels{"Utilization"}},
        {"##ProcGpuMemCard", SeriesLabels{"Memory"}},
    });
}

TEST_F(ProcessDetailsChartsRenderTest, GpuTabEmptyStatesDrawNoPlot)
{
    // Unavailable: the probe has no per-process GPU data on this system.
    {
        SCOPED_TRACE("unavailable");
        ChartInputs inputs(busyPoint, false);
        ProcessDetailsCharts charts;
        runFrame([&] { charts.renderGpuTab(inputs.ctx); });
        EXPECT_TRUE(plotsDrawn().empty());
    }
    // No readings: every retained read failed.
    {
        SCOPED_TRACE("no readings");
        ChartInputs inputs(gapPoint);
        inputs.snapshot.gpuMemoryBytes = 0;
        inputs.snapshot.gpuUtilPercent = 0.0;
        ProcessDetailsCharts charts;
        runFrame([&] { charts.renderGpuTab(inputs.ctx); });
        EXPECT_TRUE(plotsDrawn().empty());
    }
    // No usage, and again after the history window changes (the text names the window).
    {
        SCOPED_TRACE("no usage");
        ChartInputs inputs(
            [](std::size_t i)
            {
                Detail::ProcessHistoryPoint point = busyPoint(i);
                point.gpuUtil = 0.0;
                point.gpuMemory = 0.0;
                return point;
            });
        inputs.snapshot.gpuMemoryBytes = 0;
        inputs.snapshot.gpuUtilPercent = 0.0;
        ProcessDetailsCharts charts;
        runFrame([&] { charts.renderGpuTab(inputs.ctx); });
        inputs.ctx.maxHistorySeconds = 60.0;
        runFrame([&] { charts.renderGpuTab(inputs.ctx); });
        EXPECT_TRUE(plotsDrawn().empty());
    }
}

TEST_F(ProcessDetailsChartsRenderTest, GapsRenderAndTheirTooltipsShow)
{
    // Every series that can be unread is a gap at every sample: the charts still draw, and hovering
    // the newest sample shows a tooltip (its rows read N/A) rather than tripping on NaN.
    ChartInputs inputs(gapPoint);
    inputs.ctx.hasPowerUsage = true;
    ProcessDetailsCharts charts;
    const auto draw = [&]
    {
        renderOverview(charts, inputs.ctx);
    };
    draw();
    draw();
    ASSERT_EQ(plotsDrawn().size(), 4U);
    EXPECT_TRUE(hoverPlot(gapResourceSeries(), draw));
    EXPECT_TRUE(hoverPlot(cpuSeries(), draw));
    EXPECT_TRUE(hoverPlot(SeriesLabels{"Power"}, draw));
}

TEST_F(ProcessDetailsChartsRenderTest, TheMemoryChartShowsATooltip)
{
    const ChartInputs inputs(busyPoint);
    ProcessDetailsCharts charts;
    const auto draw = [&]
    {
        renderOverview(charts, inputs.ctx);
    };
    draw();
    draw();
    EXPECT_TRUE(hoverPlot(memorySeries(), draw));
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

// A history shorter than the window (60 s of 300 s) leaves the chart's left part empty: hovering
// there shows no tooltip, not the oldest sample's values as if measured then (#1931).
TEST_F(ProcessDetailsChartsRenderTest, HoveringBeforeTheFirstSampleShowsNoTooltip)
{
    const ChartInputs inputs(busyPoint);
    ProcessDetailsCharts charts;
    const auto draw = [&]
    {
        renderOverview(charts, inputs.ctx);
    };
    draw();
    draw();
    EXPECT_FALSE(hoverPlot(cpuSeries(), draw, true));
    EXPECT_FALSE(hoverPlot(memorySeries(), draw, true));
    EXPECT_FALSE(hoverPlot(resourceSeries(), draw, true));
    EXPECT_TRUE(hoverPlot(cpuSeries(), draw)); // the sampled end still has one
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

/// The text one frame of the GPU tab drew.
[[nodiscard]] std::string gpuTabText(ProcessDetailsCharts& charts, const ProcessChartContext& ctx)
{
    std::string text;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
    ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    ImGui::LogToBuffer();
    charts.renderGpuTab(ctx);
    text = GImGui->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::End();
    ImGui::Render();
    return text;
}

// A process on two GPUs gets a breakdown under the usage table, one block per GPU with its name,
// utilisation, memory (dedicated and shared) and engines; on one GPU there is none, since it would
// repeat the table (#1207).
TEST_F(ProcessDetailsChartsRenderTest, GpuTabBreaksDownAProcessOnTwoGpus)
{
    ChartInputs inputs(busyPoint);
    inputs.snapshot.perGpuUsage = {
        {.gpuId = "0",
         .gpuName = "Discrete GPU 4090",
         .isIntegrated = false,
         .utilPercent = 60.0,
         .memoryBytes = 2'000'000'000,
         .dedicatedMemoryBytes = 2'000'000'000,
         .sharedMemoryBytes = 0,
         .engines = {"3D", "Copy"}},
        {.gpuId = "1",
         .gpuName = "Integrated GPU 770",
         .isIntegrated = true,
         .utilPercent = 5.0,
         .memoryBytes = 300'000'000,
         .dedicatedMemoryBytes = 0,
         .sharedMemoryBytes = 300'000'000,
         .engines = {"Video"}},
    };
    ProcessDetailsCharts charts;
    static_cast<void>(gpuTabText(charts, inputs.ctx));
    const std::string text = gpuTabText(charts, inputs.ctx);
    EXPECT_TRUE(text.contains("Per-GPU Breakdown")) << text;
    EXPECT_TRUE(text.contains("Discrete GPU 4090")) << text;
    EXPECT_TRUE(text.contains("Integrated GPU 770")) << text;
    EXPECT_TRUE(text.contains("3D")) << text;
    EXPECT_TRUE(text.contains("Video")) << text;

    inputs.snapshot.perGpuUsage.resize(1);
    static_cast<void>(gpuTabText(charts, inputs.ctx));
    EXPECT_FALSE(gpuTabText(charts, inputs.ctx).contains("Per-GPU Breakdown"));
}

TEST_F(ProcessDetailsChartsRenderTest, TheNetworkChartsShowTooltips)
{
    ChartInputs inputs(busyPoint);
    ProcessDetailsCharts charts;
    const auto drawNetwork = [&]
    {
        runFrame([&] { charts.renderNetworkTab(inputs.ctx); });
    };
    drawNetwork();
    drawNetwork();
    EXPECT_TRUE(hoverPlot(SeriesLabels{"Sent", "Received"}, drawNetwork));
}

TEST_F(ProcessDetailsChartsRenderTest, TheGpuUsageCardListsEveryReadingAndEachGpu)
{
    // #1395: the current-values card with everything a busy multi-GPU process can show -- devices,
    // engines, encoder and decoder, dedicated and shared memory -- and the per-GPU breakdown beneath.
    ChartInputs inputs(busyPoint);
    Domain::ProcessSnapshot& proc = inputs.snapshot;
    proc.gpuDevices = "0,1";
    proc.gpuEngines = {"3D", "Video Decode"};
    proc.gpuEncoderUtil = 5.0;
    proc.gpuDecoderUtil = 7.5;
    proc.gpuDedicatedMemoryBytes = 600'000;
    proc.gpuSharedMemoryBytes = 400'000;
    proc.perGpuUsage = {
        Domain::ProcessSnapshot::PerGPUUsage{.gpuId = "0",
                                             .gpuName = "Test Discrete GPU",
                                             .isIntegrated = false,
                                             .utilPercent = 10.0,
                                             .memoryBytes = 600'000,
                                             .dedicatedMemoryBytes = 600'000,
                                             .sharedMemoryBytes = 0,
                                             .engines = {"3D"}},
        Domain::ProcessSnapshot::PerGPUUsage{.gpuId = "1",
                                             .gpuName = "Test Integrated GPU",
                                             .isIntegrated = true,
                                             .utilPercent = 2.0,
                                             .memoryBytes = 400'000,
                                             .dedicatedMemoryBytes = 0,
                                             .sharedMemoryBytes = 400'000,
                                             .engines = {"Video Decode"}},
    };
    ProcessDetailsCharts charts;
    std::string text;
    for (int frame = 0; frame < 3; ++frame)
    {
        runFrame(
            [&]
            {
                ImGui::LogToBuffer();
                charts.renderGpuTab(inputs.ctx);
                text = GImGui->LogBuffer.c_str();
                ImGui::LogFinish();
            });
    }

    for (const char* expected : {"GPU Device(s):",
                                 "0,1",
                                 "Active Engines:",
                                 "3D, Video Decode",
                                 "Video Encoder:",
                                 "5.0%",
                                 "Video Decoder:",
                                 "7.5%",
                                 "Dedicated:",
                                 "Shared:",
                                 "Per-GPU Breakdown:",
                                 "Test Discrete GPU [Discrete]",
                                 "Test Integrated GPU [Shared Memory]",
                                 "Engines:"})
    {
        EXPECT_TRUE(text.contains(expected)) << "missing \"" << expected << "\" in:\n" << text;
    }
}

/// Gives @p inputs' process the NPU use of an Intel AI Boost user, its bars taken outright.
void useTheNpu(ChartInputs& inputs)
{
    Domain::ProcessSnapshot& proc = inputs.snapshot;
    proc.npuUtilPercent = 42.0;
    proc.npuMemoryBytes = 8'388'608;
    proc.npuSharedMemoryBytes = 8'388'608;
    proc.npuDevices = "Intel(R) AI Boost";
    proc.npuEngines = {"Compute"};
    inputs.smoothed.update(proc, inputs.ctx.rateReadings, 0.0F, std::chrono::milliseconds(1000));
}

/// The text the last of @p frames frames of @p render drew.
[[nodiscard]] std::string tabText(const std::function<void()>& render, int frames = 2)
{
    std::string text;
    int frame = 0;
    while (frame < frames)
    {
        ProcessDetailsChartsRenderTest::runFrameForText(
            [&]
            {
                ImGui::LogToBuffer();
                render();
                text = GImGui->LogBuffer.c_str();
                ImGui::LogFinish();
            });
        ++frame;
    }
    return text;
}

// #1869: the GPU tab is GPU-only again: a process using the NPU has no NPU card or figures there.
TEST_F(ProcessDetailsChartsRenderTest, TheGpuTabNoLongerShowsTheNpu)
{
    ChartInputs inputs(busyPoint);
    useTheNpu(inputs);
    ProcessDetailsCharts charts;
    const std::string text = tabText([&] { charts.renderGpuTab(inputs.ctx); });
    EXPECT_TRUE(text.contains("GPU Utilization:")) << text;
    EXPECT_FALSE(text.contains("NPU")) << text;
    EXPECT_FALSE(text.contains("AI Boost")) << text;
    EXPECT_EQ(findCard("##ProcNpuUsageCard"), nullptr);
}

// #1869: the NPU tab, modeled on the GPU tab: the NPU Usage card with every NPU figure, then the
// utilization chart and the memory chart with its dedicated and shared series, each in its own card.
// #1891: as the Overview's, a GPU or NPU history chart cut off by the bottom of the pane shows its
// card's header but culls its plot, and is skipped whole rather than drawn in part.
TEST_F(ProcessDetailsChartsRenderTest, GpuAndNpuChartsCutOffByTheBottomOfThePaneAreSkippedWhole)
{
    ChartInputs gpu(busyPoint);
    ChartInputs npu(busyPoint);
    useTheNpu(npu);
    ProcessDetailsCharts charts;
    const auto renderBoth = [&](float offset)
    {
        runFrame(
            [&]
            {
                ImGui::Dummy(ImVec2(1.0F, offset));
                charts.renderGpuTab(gpu.ctx);
            });
        runFrame(
            [&]
            {
                ImGui::Dummy(ImVec2(1.0F, offset));
                charts.renderNpuTab(npu.ctx);
            });
    };
    renderBoth(1.0F);
    renderBoth(1.0F);
    const auto full = plotsDrawn();
    ASSERT_FALSE(full.empty());

    for (int step = 0; static_cast<float>(step) * 6.0F < DISPLAY_HEIGHT; ++step)
    {
        const float offset = 1.0F + (static_cast<float>(step) * 6.0F);
        renderBoth(offset);
        for (const SeriesLabels& plot : plotsDrawn())
        {
            EXPECT_TRUE(plot.empty() || std::ranges::find(full, plot) != full.end()) << "at offset " << offset;
        }
    }
}

TEST_F(ProcessDetailsChartsRenderTest, TheNpuTabShowsItsUsageCardAndCharts)
{
    ChartInputs inputs(busyPoint);
    useTheNpu(inputs);
    ProcessDetailsCharts charts;
    std::string text;
    int frame = 0;
    while (frame < 4) // The fill layout measures from the previous frame
    {
        text = tabText([&] { charts.renderNpuTab(inputs.ctx); }, 1);
        if (const ImGuiWindow* card = findCard("##ProcNpuUsageCard"); card != nullptr)
        {
            EXPECT_TRUE(std::isfinite(card->ContentSize.x)) << "frame " << frame;
        }
        ++frame;
    }
    for (const char* expected : {"NPU Usage",
                                 "NPU Utilization:",
                                 "42.0%",
                                 "NPU Memory:",
                                 "8.0 MiB",
                                 "Dedicated:",
                                 "Shared:",
                                 "NPU Device(s):",
                                 "Intel(R) AI Boost",
                                 "Active Engines:",
                                 "Compute",
                                 "NPU Utilization History",
                                 "NPU Memory History"})
    {
        EXPECT_TRUE(text.contains(expected)) << "missing \"" << expected << "\" in:\n" << text;
    }
    EXPECT_FALSE(text.contains("GPU")) << text;

    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 2U);
    EXPECT_EQ(plots[0], (SeriesLabels{"Utilization"}));
    EXPECT_EQ(plots[1], (SeriesLabels{"Dedicated", "Shared"}));

    const ImGuiWindow* usage = findCard("##ProcNpuUsageCard");
    const ImGuiWindow* utilization = findCard("##ProcNpuUtilCard");
    ASSERT_NE(usage, nullptr);
    ASSERT_NE(utilization, nullptr);
    EXPECT_LE(usage->Pos.y + usage->Size.y, utilization->Pos.y); // The values above the charts
    expectChartsInFillingCards({
        {"##ProcNpuUtilCard", SeriesLabels{"Utilization"}},
        {"##ProcNpuMemCard", SeriesLabels{"Dedicated", "Shared"}},
    });
}

TEST_F(ProcessDetailsChartsRenderTest, TheNpuMemoryChartShowsATooltip)
{
    ChartInputs inputs(busyPoint);
    useTheNpu(inputs);
    ProcessDetailsCharts charts;
    const auto drawNpu = [&]
    {
        runFrame([&] { charts.renderNpuTab(inputs.ctx); });
    };
    drawNpu();
    drawNpu();
    EXPECT_TRUE(hoverPlot(SeriesLabels{"Dedicated", "Shared"}, drawNpu));
}

// #1869: a process whose NPU history is all zero -- a GPU-busy one included -- gets the muted
// explanation instead of charts, naming the history window; so does one whose reads failed, or a
// system without per-process data, each in its own words.
TEST_F(ProcessDetailsChartsRenderTest, TheNpuTabExplainsWhyItHasNoCharts)
{
    {
        SCOPED_TRACE("idle");
        // Busy on the GPU, never on the NPU
        ChartInputs idle(
            [](std::size_t i)
            {
                Detail::ProcessHistoryPoint point = busyPoint(i);
                point.npuUtil = 0.0;
                point.npuDedicatedMemory = 0.0;
                point.npuSharedMemory = 0.0;
                return point;
            });
        ProcessDetailsCharts charts;
        std::string text = tabText([&] { charts.renderNpuTab(idle.ctx); });
        EXPECT_TRUE(text.contains("No NPU usage")) << text;
        EXPECT_TRUE(text.contains("This process hasn't used the NPU in its retained history (up to 5m).")) << text;
        EXPECT_TRUE(plotsDrawn().empty());
        EXPECT_EQ(findCard("##ProcNpuUsageCard"), nullptr);

        idle.ctx.maxHistorySeconds = 60.0; // The text follows the window
        text = tabText([&] { charts.renderNpuTab(idle.ctx); });
        EXPECT_TRUE(text.contains("(up to 1m)")) << text;
    }
    {
        SCOPED_TRACE("no readings");
        ChartInputs inputs(gapPoint);
        ProcessDetailsCharts charts;
        const std::string text = tabText([&] { charts.renderNpuTab(inputs.ctx); });
        EXPECT_TRUE(text.contains("No NPU readings yet")) << text;
        EXPECT_TRUE(plotsDrawn().empty());
    }
    {
        SCOPED_TRACE("unavailable");
        ChartInputs inputs(busyPoint, false);
        useTheNpu(inputs);
        ProcessDetailsCharts charts;
        const std::string text = tabText([&] { charts.renderNpuTab(inputs.ctx); });
        EXPECT_TRUE(text.contains("Per-process NPU usage is not available")) << text;
        EXPECT_TRUE(plotsDrawn().empty());
    }
}

// #1210, #1487: the GPU tab's "No GPU usage" text names the history window, and the cached text is
// rebuilt when the window changes (keyed by whole milliseconds, not compared as floats).
TEST_F(ProcessDetailsChartsRenderTest, TheGpuTabExplainsWhyItHasNoChartsAndFollowsTheWindow)
{
    ChartInputs idle(
        [](std::size_t i)
        {
            Detail::ProcessHistoryPoint point = busyPoint(i);
            point.gpuUtil = 0.0;
            point.gpuMemory = 0.0;
            return point;
        });
    idle.snapshot.gpuMemoryBytes = 0;
    idle.snapshot.gpuUtilPercent = 0.0;
    ProcessDetailsCharts charts;
    std::string text = tabText([&] { charts.renderGpuTab(idle.ctx); });
    EXPECT_TRUE(text.contains("No GPU usage")) << text;
    EXPECT_TRUE(text.contains("This process has not used a GPU in its retained history (up to 5m).")) << text;
    EXPECT_TRUE(plotsDrawn().empty());

    idle.ctx.maxHistorySeconds = 60.0;
    text = tabText([&] { charts.renderGpuTab(idle.ctx); });
    EXPECT_TRUE(text.contains("(up to 1m).")) << text;
    EXPECT_FALSE(text.contains("(up to 5m)")) << text;

    idle.ctx.maxHistorySeconds = 90.0;
    text = tabText([&] { charts.renderGpuTab(idle.ctx); });
    EXPECT_TRUE(text.contains("(up to 1m 30s).")) << text;
}

TEST_F(ProcessDetailsChartsRenderTest, OneGpuHasNoBreakdownAndNoEncoderRows)
{
    ChartInputs inputs(busyPoint);
    inputs.snapshot.gpuDedicatedMemoryBytes = inputs.snapshot.gpuMemoryBytes; // all of it dedicated
    inputs.snapshot.perGpuUsage = {Domain::ProcessSnapshot::PerGPUUsage{.gpuId = "0",
                                                                        .gpuName = "Only GPU",
                                                                        .isIntegrated = false,
                                                                        .utilPercent = 12.0,
                                                                        .memoryBytes = 1'000'000,
                                                                        .dedicatedMemoryBytes = 1'000'000,
                                                                        .sharedMemoryBytes = 0,
                                                                        .engines = {}}};
    ProcessDetailsCharts charts;
    std::string text;
    for (int frame = 0; frame < 2; ++frame)
    {
        runFrame(
            [&]
            {
                ImGui::LogToBuffer();
                charts.renderGpuTab(inputs.ctx);
                text = GImGui->LogBuffer.c_str();
                ImGui::LogFinish();
            });
    }
    EXPECT_TRUE(text.contains("GPU Utilization:")) << text;
    EXPECT_FALSE(text.contains("Per-GPU Breakdown:")) << text; // it would repeat the table (#1207)
    EXPECT_FALSE(text.contains("Video Encoder:")) << text;
    EXPECT_FALSE(text.contains("Dedicated:")) << text; // the total already is the dedicated bytes
}

/// busyPoint() with a thread count that swings between 100 and 120 and a handle count fifty times
/// that, as a real process's do (#1916).
[[nodiscard]] Detail::ProcessHistoryPoint manyHandlesPoint(std::size_t i)
{
    Detail::ProcessHistoryPoint point = busyPoint(i);
    point.threads = 100.0 + static_cast<double>((i * 7U) % 21U); // 100..120
    point.handles = 6000.0 + static_cast<double>(i);
    point.gdiObjects = 60.0;
    return point;
}

/// The Overview's plot whose series are @p labels, or null.
[[nodiscard]] ImPlotPlot* findPlot(const SeriesLabels& labels)
{
    ImPlotContext& context = *ImPlot::GetCurrentContext();
    int i = 0;
    while (i < context.Plots.GetBufSize())
    {
        ImPlotPlot* plot = context.Plots.GetByIndex(i);
        ++i;
        if (plot == nullptr)
        {
            continue;
        }
        SeriesLabels found;
        int item = 0;
        while (item < plot->Items.GetLegendCount())
        {
            found.emplace(plot->Items.GetLegendLabel(item));
            ++item;
        }
        if (found == labels)
        {
            return plot;
        }
    }
    return nullptr;
}

// #1916: the Threads line is read on an axis of its own. A process holds tens of times more handles
// than threads, and while the two shared the left axis it was sized to the handles and the Threads
// line lay flat on the baseline. Now the left axis is sized to the threads (and GDI objects, of the
// same order), so a thread count swinging between 100 and 120 moves its line visibly, and the handles
// have their own right-hand axis (Y3), outside Page Faults' (Y2).
TEST_F(ProcessDetailsChartsRenderTest, TheThreadsLineIsNotFlattenedByTheHandles)
{
    const ChartInputs inputs(manyHandlesPoint);
    ProcessDetailsCharts charts;
    renderOverview(charts, inputs.ctx);
    renderOverview(charts, inputs.ctx);

    ImPlotPlot* plot = findPlot(resourceSeries());
    ASSERT_NE(plot, nullptr);
    const ImPlotAxis& threadAxis = plot->Axes[ImAxis_Y1];
    const ImPlotAxis& faultAxis = plot->Axes[ImAxis_Y2];
    const ImPlotAxis& handleAxis = plot->Axes[ImAxis_Y3];
    ASSERT_TRUE(faultAxis.Enabled);
    ASSERT_TRUE(handleAxis.Enabled);

    // The left axis covers the threads with some headroom, not the handles.
    EXPECT_DOUBLE_EQ(threadAxis.Range.Min, 0.0);
    EXPECT_GE(threadAxis.Range.Max, 120.0);
    EXPECT_LT(threadAxis.Range.Max, 200.0);
    // The handles' axis covers the handles.
    EXPECT_DOUBLE_EQ(handleAxis.Range.Min, 0.0);
    EXPECT_GE(handleAxis.Range.Max, 6000.0 + static_cast<double>(HISTORY_POINTS));
    // Both right-hand axes are on the right: their tick labels sit right of the plot.
    EXPECT_TRUE(faultAxis.IsOpposite());
    EXPECT_TRUE(handleAxis.IsOpposite());

    // The thread count's swing of 20 spans a readable share of the plot's height, where on the
    // handles' scale it was well under one percent of it -- and the line sits well above the baseline.
    const float plotHeight = plot->PlotRect.GetHeight();
    ASSERT_GT(plotHeight, 0.0F);
    const double swingShare = 20.0 / threadAxis.Range.Size();
    EXPECT_GT(swingShare, 0.10);
    EXPECT_GT(swingShare * static_cast<double>(plotHeight), 10.0);
    EXPECT_GT(100.0 / threadAxis.Range.Size(), 0.5);

    // Steady, both lines sit near the top of their own axes: Threads is drawn after Handles, on top.
    int threadsAt = -1;
    int handlesAt = -1;
    int item = 0;
    while (item < plot->Items.GetLegendCount())
    {
        const std::string_view label = plot->Items.GetLegendLabel(item);
        threadsAt = (label == "Threads") ? item : threadsAt;
        handlesAt = (label == HANDLES_LABEL) ? item : handlesAt;
        ++item;
    }
    ASSERT_GE(handlesAt, 0);
    EXPECT_GT(threadsAt, handlesAt);
}

/// The first plot ImPlot knows whose series include @p label, or null.
[[nodiscard]] ImPlotPlot* findPlotWith(std::string_view label)
{
    ImPlotContext& context = *ImPlot::GetCurrentContext();
    for (int i = 0; i < context.Plots.GetBufSize(); ++i)
    {
        ImPlotPlot* plot = context.Plots.GetByIndex(i);
        if (plot == nullptr)
        {
            continue;
        }
        for (int item = 0; item < plot->Items.GetLegendCount(); ++item)
        {
            if (std::string_view(plot->Items.GetLegendLabel(item)) == label)
            {
                return plot;
            }
        }
    }
    return nullptr;
}

/// @p history's time axis as the charts build it, seconds before its newest sample. Every sample of
/// ChartInputs' history is inside the 300 s window, so the window's maxima are the same as at any
/// frame's "now".
[[nodiscard]] std::vector<double> timeAxisOf(const Detail::ProcessDetailsHistory& history)
{
    const std::span<const double> stamps = history.timestamps();
    std::vector<double> x;
    x.reserve(stamps.size());
    for (const double stamp : stamps)
    {
        x.push_back(stamp - stamps.back());
    }
    return x;
}

/// Each chart's Y range, against what @p expected says it is.
void expectAxisMax(const char* chart, ImPlotPlot* plot, ImAxis axis, double expected)
{
    ASSERT_NE(plot, nullptr) << chart;
    ASSERT_TRUE(plot->Axes[axis].Enabled) << chart;
    EXPECT_DOUBLE_EQ(plot->Axes[axis].Range.Max, expected) << chart;
}

// The charts size their axes from the window's maxima kept between frames (windowSeriesMax(), #843),
// through the helpers' "from the window's max" form; the helpers' tests use the form that takes the
// series. This pins each call site to the series it must cover: every axis the Overview, Network and
// I/O, GPU and NPU tabs draw has exactly the bound the series form gives, so a series dropped from
// one (say Swap from Memory) fails here. On a chart's first frames its eased bound is its target.
//
// busyPoint() with series @p Big made the largest on its axis, so dropping it from its axis changes
// the bound: the CPU series to half again the total (still under 100 %), the others to far above
// every other series.
template<ProcessSeries Big> [[nodiscard]] Detail::ProcessHistoryPoint dominantPoint(std::size_t i)
{
    Detail::ProcessHistoryPoint point = busyPoint(i);
    constexpr bool CPU_SERIES = (Big == ProcessSeries::CpuTotal) || (Big == ProcessSeries::CpuUser) || (Big == ProcessSeries::CpuSystem);
    point.*Detail::PROCESS_SERIES_FIELDS[static_cast<std::size_t>(Big)] =
        CPU_SERIES ? 1.5 * point.cpuTotal : (static_cast<double>(i) + 1.0) * 1.0e9;
    return point;
}

TEST_F(ProcessDetailsChartsRenderTest, EveryAxisCoversTheSeriesItsHelperNames)
{
    struct Variant
    {
        const char* name;
        Detail::ProcessHistoryPoint (*point)(std::size_t);
    };
    // The plain history, then each series that does not already set its axis made the one that does.
    const std::array variants{
        Variant{.name = "busy", .point = busyPoint},
        Variant{.name = "User", .point = dominantPoint<ProcessSeries::CpuUser>},
        Variant{.name = "System", .point = dominantPoint<ProcessSeries::CpuSystem>},
        Variant{.name = "Shared", .point = dominantPoint<ProcessSeries::MemoryShared>},
        Variant{.name = "Private", .point = dominantPoint<ProcessSeries::MemoryPrivate>},
        Variant{.name = "Swap", .point = dominantPoint<ProcessSeries::MemorySwap>},
        Variant{.name = "GDI Objects", .point = dominantPoint<ProcessSeries::GdiObjects>},
        Variant{.name = "I/O Read", .point = dominantPoint<ProcessSeries::IoRead>},
        Variant{.name = "Net Sent", .point = dominantPoint<ProcessSeries::NetSent>},
        Variant{.name = "NPU Dedicated", .point = dominantPoint<ProcessSeries::NpuDedicatedMemory>},
    };
    bool first = true;
    for (const Variant& variant : variants)
    {
        SCOPED_TRACE(variant.name);
        if (!first)
        {
            // A new ImGui context: every chart's first frame again, so every bound is its target.
            TearDown();
            SetUp();
        }
        first = false;
        expectEveryAxisCoversItsSeries(variant.point);
    }
}

void ProcessDetailsChartsRenderTest::expectEveryAxisCoversItsSeries(Detail::ProcessHistoryPoint (*makePoint)(std::size_t))
{
    using UI::Widgets::percentAxisUpperBound;
    using UI::Widgets::rateAxisUpperBound;
    ChartInputs inputs(makePoint);
    useTheNpu(inputs);
    inputs.ctx.hasPowerUsage = true;
    inputs.ctx.hasPrivateBytes = true;
    inputs.ctx.hasSwapBytes = true;
    const auto& history = inputs.history;
    const auto& smoothed = inputs.smoothed;
    const std::vector<double> xs = timeAxisOf(history);
    const std::span<const double> x(xs);
    const double xMin = -inputs.ctx.maxHistorySeconds;
    const auto series = [&history](ProcessSeries which)
    {
        return history.series(which);
    };

    ProcessDetailsCharts charts;
    renderOverview(charts, inputs.ctx);
    renderOverview(charts, inputs.ctx);
    expectAxisMax(
        "CPU",
        findPlotWith("Total"),
        ImAxis_Y1,
        percentAxisUpperBound(Detail::cpuAxisDataMax(
            x, xMin, series(ProcessSeries::CpuTotal), series(ProcessSeries::CpuUser), series(ProcessSeries::CpuSystem), smoothed)));
    ImPlotPlot* memory = findPlotWith("Virtual →");
    expectAxisMax("Memory",
                  memory,
                  ImAxis_Y1,
                  rateAxisUpperBound(Detail::memoryAxisDataMax(x,
                                                               xMin,
                                                               series(ProcessSeries::MemoryUsed),
                                                               series(ProcessSeries::MemoryShared),
                                                               smoothed,
                                                               series(ProcessSeries::MemoryPrivate),
                                                               series(ProcessSeries::MemorySwap)),
                                     UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES));
    expectAxisMax("Virtual",
                  memory,
                  ImAxis_Y2,
                  rateAxisUpperBound(Detail::virtualAxisDataMax(x, xMin, series(ProcessSeries::Virtual), smoothed),
                                     UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES));
    expectAxisMax("Power",
                  findPlotWith("Power"),
                  ImAxis_Y1,
                  rateAxisUpperBound(Detail::powerAxisDataMax(x, xMin, series(ProcessSeries::Power), smoothed),
                                     UI::Widgets::RATE_AXIS_MIN_SPAN_WATTS));
    ImPlotPlot* resources = findPlotWith("Threads");
#ifdef _WIN32
    const double threads =
        Detail::threadAxisDataMaxWithGdi(x, xMin, series(ProcessSeries::Threads), series(ProcessSeries::GdiObjects), smoothed);
#else
    const double threads = Detail::threadAxisDataMax(x, xMin, series(ProcessSeries::Threads), smoothed);
#endif
    expectAxisMax("Threads", resources, ImAxis_Y1, rateAxisUpperBound(threads, UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT));
    expectAxisMax("Page Faults",
                  resources,
                  ImAxis_Y2,
                  rateAxisUpperBound(Detail::faultAxisDataMax(x, xMin, series(ProcessSeries::PageFaults), smoothed),
                                     UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT));
    expectAxisMax("Handles",
                  resources,
                  ImAxis_Y3,
                  rateAxisUpperBound(Detail::handleAxisDataMax(x, xMin, series(ProcessSeries::Handles), smoothed),
                                     UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT));

    runFrame([&] { charts.renderNetworkTab(inputs.ctx); });
    runFrame([&] { charts.renderNetworkTab(inputs.ctx); });
    expectAxisMax(
        "I/O",
        findPlotWith("Write"),
        ImAxis_Y1,
        rateAxisUpperBound(Detail::ioAxisDataMax(x, xMin, series(ProcessSeries::IoRead), series(ProcessSeries::IoWrite), smoothed),
                           UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC));
    expectAxisMax("Network",
                  findPlotWith("Received"),
                  ImAxis_Y1,
                  rateAxisUpperBound(
                      Detail::networkAxisDataMax(x, xMin, series(ProcessSeries::NetSent), series(ProcessSeries::NetReceived), smoothed),
                      UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC));

    runFrame([&] { charts.renderGpuTab(inputs.ctx); });
    runFrame([&] { charts.renderGpuTab(inputs.ctx); });
    expectAxisMax("GPU Memory",
                  findPlot(SeriesLabels{"Memory"}),
                  ImAxis_Y1,
                  rateAxisUpperBound(Detail::gpuMemoryAxisDataMax(x, xMin, series(ProcessSeries::GpuMemory), smoothed),
                                     UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES));

    for (int frame = 0; frame < 4; ++frame) // The fill layout measures from the previous frame
    {
        runFrame([&] { charts.renderNpuTab(inputs.ctx); });
    }
    expectAxisMax(
        "NPU Memory",
        findPlotWith("Dedicated"),
        ImAxis_Y1,
        rateAxisUpperBound(Detail::npuMemoryAxisDataMax(
                               x, xMin, series(ProcessSeries::NpuDedicatedMemory), series(ProcessSeries::NpuSharedMemory), smoothed),
                           UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES));
}

#ifdef _WIN32
/// busyPoint() with the newest few GDI readings missing: the series is drawn, its newest value is N/A.
[[nodiscard]] Detail::ProcessHistoryPoint newestGdiGapPoint(std::size_t i)
{
    Detail::ProcessHistoryPoint point = busyPoint(i);
    if (i + 3 >= HISTORY_POINTS)
    {
        point.gdiObjects = NOT_A_NUMBER;
    }
    return point;
}

// Windows only (#1000, #1395 Windows half): the Resources tooltip lists the GDI Objects series beside
// Threads and Handles, a reading and a missing one alike.
TEST_F(ProcessDetailsChartsRenderTest, TheResourcesTooltipListsGdiObjects)
{
    for (Detail::ProcessHistoryPoint (*makePoint)(std::size_t) : {busyPoint, newestGdiGapPoint})
    {
        const ChartInputs inputs(makePoint);
        ProcessDetailsCharts charts;
        const auto draw = [&]
        {
            renderOverview(charts, inputs.ctx);
        };
        draw();
        draw();
        const auto plots = plotsDrawn();
        ASSERT_EQ(plots.size(), 3U);
        EXPECT_EQ(plots[2], resourceSeries()); // GDI Objects is drawn: it has readings
        EXPECT_TRUE(hoverPlot(resourceSeries(), draw));
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
}
#endif

/// The vertices drawn in the theme's N/A NowBar hatch (UI::ColorScheme::unavailableHatch, #1935) in
/// the card @p id in the last frame. The stub scheme gives the hatch a colour nothing else uses, so
/// any are an unavailable bar's; a measured 0 draws its zero bar in the series' colour instead.
[[nodiscard]] int hatchVerticesIn(const std::string& id)
{
    const ImGuiWindow* card = findCard(id);
    if (card == nullptr || !card->Active)
    {
        ADD_FAILURE() << "no card " << id << " this frame";
        return -1;
    }
    const ImU32 hatch = ImGui::ColorConvertFloat4ToU32(UI::Theme::get().scheme().unavailableHatch);
    int count = 0;
    for (const ImDrawVert& vertex : card->DrawList->VtxBuffer)
    {
        count += (vertex.col == hatch) ? 1 : 0;
    }
    return count;
}

/// Takes @p inputs' snapshot and readings into its smoothed values afresh, as the panel does for a
/// newly selected process.
void resample(ChartInputs& inputs)
{
    inputs.smoothed.reset();
    inputs.smoothed.update(inputs.snapshot, inputs.ctx.rateReadings, 0.0F, std::chrono::milliseconds(1000));
}

/// A Resources chart in which every count and rate is a reading, the ones that can be a measured 0 at 0.
void readEveryResource(ChartInputs& inputs)
{
    inputs.snapshot.threadCount = 4; // A running process has a thread: 0 is not a reading
    inputs.snapshot.handleCountAvailable = true;
    inputs.snapshot.handleCount = 0;
    inputs.snapshot.gdiObjectCount = 0;
    inputs.snapshot.pageFaultsPerSec = 0.0;
    inputs.ctx.hasPageFaults = true;
    resample(inputs);
}

// #1935: a Resources bar whose count could not be read -- Threads, Handles/FDs, GDI Objects -- or
// whose rate the platform has no counter for (Page Faults) is drawn as the hatched N/A bar, as #1918
// drew the Network and I/O ones, while a measured 0 is drawn as a zero bar.
TEST_F(ProcessDetailsChartsRenderTest, AResourceBarWithNoReadingIsDrawnUnavailable)
{
    {
        ChartInputs inputs(busyPoint);
        readEveryResource(inputs);
        ProcessDetailsCharts charts;
        renderOverview(charts, inputs.ctx);
        renderOverview(charts, inputs.ctx);
        EXPECT_EQ(hatchVerticesIn("##ProcResourcesCard"), 0) << "measured zeros are not N/A";
    }

    struct Unread
    {
        const char* bar;
        void (*unread)(ChartInputs&);
    };
    const std::vector<Unread> cases{
        {.bar = "Threads", .unread = [](ChartInputs& in) { in.snapshot.threadCount = 0; }},
        {.bar = "Handles", .unread = [](ChartInputs& in) { in.snapshot.handleCountAvailable = false; }},
        {.bar = "Page Faults", .unread = [](ChartInputs& in) { in.ctx.hasPageFaults = false; }},
#ifdef _WIN32
        {.bar = "GDI Objects", .unread = [](ChartInputs& in) { in.snapshot.gdiObjectCount.reset(); }},
#endif
    };
    for (const Unread& unreadCase : cases)
    {
        ChartInputs inputs(busyPoint);
        readEveryResource(inputs);
        unreadCase.unread(inputs);
        resample(inputs);
        ProcessDetailsCharts charts;
        renderOverview(charts, inputs.ctx);
        renderOverview(charts, inputs.ctx);
        EXPECT_GT(hatchVerticesIn("##ProcResourcesCard"), 0) << unreadCase.bar;
    }
}

// #1935: the GPU tab's Utilization and Memory bars, for a sample whose GPU figures were not read, are
// the hatched N/A bar; a measured 0 % and 0 B are zero bars.
TEST_F(ProcessDetailsChartsRenderTest, AGpuBarWithNoReadingIsDrawnUnavailable)
{
    const auto draw = [](ChartInputs& inputs, Detail::SampleRateReadings readings)
    {
        inputs.snapshot.gpuUtilPercent = 0.0;
        inputs.snapshot.gpuMemoryBytes = 0;
        inputs.ctx.rateReadings = readings;
        resample(inputs);
        ProcessDetailsCharts charts;
        runFrame([&] { charts.renderGpuTab(inputs.ctx); });
        runFrame([&] { charts.renderGpuTab(inputs.ctx); });
    };
    constexpr Detail::SampleRateReadings READ{
        .io = true, .network = true, .gpuPerProcess = true, .gpuUtilization = true, .gpuSupported = true};
    {
        ChartInputs inputs(busyPoint);
        draw(inputs, READ);
        EXPECT_EQ(hatchVerticesIn("##ProcGpuUtilCard"), 0);
        EXPECT_EQ(hatchVerticesIn("##ProcGpuMemCard"), 0);
    }
    {
        // Memory but not utilization (NVML on Linux).
        ChartInputs inputs(busyPoint);
        Detail::SampleRateReadings noUtilization = READ;
        noUtilization.gpuUtilization = false;
        draw(inputs, noUtilization);
        EXPECT_GT(hatchVerticesIn("##ProcGpuUtilCard"), 0);
        EXPECT_EQ(hatchVerticesIn("##ProcGpuMemCard"), 0);
    }
    {
        // A failed read: neither, though the probe supports them.
        ChartInputs inputs(busyPoint);
        Detail::SampleRateReadings failed = READ;
        failed.gpuPerProcess = false;
        failed.gpuUtilization = false;
        draw(inputs, failed);
        EXPECT_GT(hatchVerticesIn("##ProcGpuUtilCard"), 0);
        EXPECT_GT(hatchVerticesIn("##ProcGpuMemCard"), 0);
    }
}

// #1935: the NPU tab's Dedicated and Shared memory bars likewise. An NPU that maps system RAM reads a
// measured 0 B dedicated, which stays a zero bar.
TEST_F(ProcessDetailsChartsRenderTest, AnNpuMemoryBarWithNoReadingIsDrawnUnavailable)
{
    for (const bool read : {true, false})
    {
        ChartInputs inputs(busyPoint);
        useTheNpu(inputs);
        inputs.ctx.rateReadings.gpuPerProcess = read;
        resample(inputs);
        ProcessDetailsCharts charts;
        runFrame([&] { charts.renderNpuTab(inputs.ctx); });
        runFrame([&] { charts.renderNpuTab(inputs.ctx); });
        if (read)
        {
            EXPECT_EQ(hatchVerticesIn("##ProcNpuMemCard"), 0);
        }
        else
        {
            EXPECT_GT(hatchVerticesIn("##ProcNpuMemCard"), 0);
        }
        EXPECT_EQ(hatchVerticesIn("##ProcNpuUtilCard"), 0) << "read=" << read; // its utilization is still read
    }
}

} // namespace
} // namespace App
