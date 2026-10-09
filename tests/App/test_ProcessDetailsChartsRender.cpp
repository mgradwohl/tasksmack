/// @file test_ProcessDetailsChartsRender.cpp
/// @brief Process Details' chart tabs rendered headless (#1179, slice 5): each tab draws the plots
/// and series it drew inside ProcessDetailsPanel -- by their ImPlot legend labels -- its empty states
/// draw no plot, the optional series (Shared, Power) follow the capabilities, and a gap-only
/// history and a hovered gap render (the tooltip shows N/A rather than tripping on NaN).

#include "App/Panels/ProcessDetailsCharts.h"
#include "App/Panels/ProcessDetailsHistory.h"
#include "App/Panels/ProcessDetailsPanel_HistoryHelpers.h"
#include "App/Panels/ProcessSmoothedUsage.h"
#include "Domain/ProcessSnapshot.h"
#include "UI/ChartWidgets.h"
#include "UI/FillPlotLayout.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <implot_internal.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <set>
#include <string>
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
        const Detail::SampleRateReadings readings{
            .io = true, .network = true, .gpuPerProcess = gpuSupported, .gpuUtilization = gpuSupported, .gpuSupported = gpuSupported};
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

    /// Hovers the centre of the plot whose series are @p labels and renders @p draw two more frames.
    /// Returns whether a tooltip window was shown.
    [[nodiscard]] static bool hoverPlot(const SeriesLabels& labels, const std::function<void()>& draw)
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
            ImGui::GetIO().AddMousePosEvent(rect.Max.x - 2.0F, (rect.Min.y + rect.Max.y) * 0.5F);
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

// The series each Overview chart draws. Functions, not statics: building a std::set can throw.
[[nodiscard]] SeriesLabels resourceSeries()
{
#ifdef _WIN32
    return {"Threads", "Handles", "Page Faults →", "GDI Objects"};
#else
    return {"Threads", "FDs", "Page Faults →"};
#endif
}

/// resourceSeries() for gapPoint()'s history. A GDI series with no reading at all is not drawn
/// (#1000), so on Windows it has no GDI Objects entry.
[[nodiscard]] SeriesLabels gapResourceSeries()
{
#ifdef _WIN32
    return {"Threads", "Handles", "Page Faults →"};
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
    inputs.ctx.hasSharedMemory = false; // Windows: no Shared line (#1035)
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

// The child window drawn for the card @p id (an ImGui child is named "<parent>/<id>_<hash>").
[[nodiscard]] const ImGuiWindow* findCard(const std::string& id)
{
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    for (const ImGuiWindow* window : g.Windows)
    {
        if ((window->Flags & ImGuiWindowFlags_ChildWindow) != 0 && std::string(window->Name).find("/" + id + "_") != std::string::npos)
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

TEST_F(ProcessDetailsChartsRenderTest, AMissingInputDrawsNothing)
{
    ChartInputs inputs(busyPoint);
    inputs.ctx.smoothed = nullptr;
    ProcessDetailsCharts charts;
    renderOverview(charts, inputs.ctx);
    runFrame([&] { charts.renderNetworkTab(inputs.ctx); });
    runFrame([&] { charts.renderGpuTab(inputs.ctx); });
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

} // namespace
} // namespace App
