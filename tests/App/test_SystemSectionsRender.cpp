/// @file test_SystemSectionsRender.cpp
/// @brief The System Metrics sections that were never linked into the tests (#1395): Memory & Swap,
/// CPU Cores, Disk I/O and Network. Their smoothing helpers are checked against computeAlpha()
/// directly, and
/// each section is rendered headless to check the plots and series it draws, its empty states, the
/// per-core and per-disk grids, and the per-disk smoothing map it maintains across frames.

#include "App/Panels/CpuCoresSection.h"
#include "App/Panels/MemorySection.h"
#include "App/Panels/NetInterfaceUtils.h"
#include "App/Panels/NetworkSection.h"
#include "App/Panels/StorageSection.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SharedHistory.h"
#include "Domain/StorageModel.h"
#include "Domain/StorageSnapshot.h"
#include "Domain/SystemModel.h"
#include "Domain/SystemSnapshot.h"
#include "UI/ChartSmoothing.h"
#include "UI/ChartWidgets.h"
#include "UI/FillPlotLayout.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <implot_internal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace App
{
namespace
{

constexpr float DISPLAY_WIDTH = 1600.0F;
constexpr float DISPLAY_HEIGHT = 1200.0F;
constexpr std::size_t HISTORY_POINTS = 30;
constexpr std::chrono::milliseconds REFRESH{Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS};
constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();

using SeriesLabels = std::set<std::string>;

/// A published-history view that owns @p values, like SharedHistoryBuffer::view() hands out.
template<typename T> [[nodiscard]] Domain::HistoryView<T> viewOf(std::vector<T> values)
{
    auto owner = std::make_shared<const std::vector<T>>(std::move(values));
    const std::span<const T> span(*owner);
    return Domain::HistoryView<T>(std::move(owner), span);
}

/// HISTORY_POINTS timestamps one second apart on historyFrameNowSeconds()'s clock (steady_clock
/// seconds; that function itself may only be called inside a frame), the newest at now.
[[nodiscard]] std::vector<double> timestampsToNow()
{
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::vector<double> times;
    times.reserve(HISTORY_POINTS);
    for (std::size_t i = 0; i < HISTORY_POINTS; ++i)
    {
        times.push_back(now - static_cast<double>(HISTORY_POINTS - 1 - i));
    }
    return times;
}

/// HISTORY_POINTS samples of @p value.
template<typename T> [[nodiscard]] std::vector<T> constant(T value)
{
    return std::vector<T>(HISTORY_POINTS, value);
}

/// Whether @p lines has a line equal to @p wanted; on failure the message lists every line.
[[nodiscard]] ::testing::AssertionResult hasLine(const std::vector<std::string>& lines, const std::string& wanted)
{
    if (std::ranges::find(lines, wanted) != lines.end())
    {
        return ::testing::AssertionSuccess() << "found \"" << wanted << "\"";
    }
    ::testing::AssertionResult result = ::testing::AssertionFailure() << "no line \"" << wanted << "\" in:";
    for (const std::string& line : lines)
    {
        result << "\n  " << line;
    }
    return result;
}

class SystemSectionsRenderTest : public ::testing::Test
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

    /// One frame of a window filling the display, running @p body inside it.
    static void runFrame(const std::function<void()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("System", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }

    /// Runs one frame of @p body and returns all the text it drew, captured through ImGui's own
    /// logging (LogToBuffer(), which also turns clipping off) as the other headless view tests do.
    /// Text drawn straight onto a draw list (a chart's collecting hint) is not captured.
    [[nodiscard]] static std::string renderAndCapture(const std::function<void()>& body)
    {
        std::string captured;
        runFrame(
            [&]
            {
                ImGui::LogToBuffer();
                body();
                captured = GImGui->LogBuffer.c_str();
                ImGui::LogFinish();
            });
        return captured;
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

    /// Moves the mouse to the right edge of the first plot ImPlot knows and renders @p draw twice.
    /// Returns whether a tooltip window was shown.
    [[nodiscard]] static bool hoverFirstPlot(const std::function<void()>& draw)
    {
        ImPlotContext& context = *ImPlot::GetCurrentContext();
        if (context.Plots.GetBufSize() == 0 || context.Plots.GetByIndex(0) == nullptr)
        {
            ADD_FAILURE() << "no plot to hover";
            return false;
        }
        const ImRect rect = context.Plots.GetByIndex(0)->PlotRect;
        ImGui::GetIO().AddMousePosEvent(rect.Max.x - 2.0F, (rect.Min.y + rect.Max.y) * 0.5F);
        draw();
        draw();
        const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
        return tooltip != nullptr && tooltip->Active;
    }

    /// Like hoverFirstPlot(), for a @p section drawn inside runFrame(): returns every line of text the
    /// hovered frame drew, each trimmed of surrounding whitespace, tooltip rows included. Empty (and a
    /// failure) when no tooltip was shown.
    [[nodiscard]] static std::vector<std::string> hoverFirstPlotAndCaptureLines(const std::function<void()>& section)
    {
        ImPlotContext& context = *ImPlot::GetCurrentContext();
        if (context.Plots.GetBufSize() == 0 || context.Plots.GetByIndex(0) == nullptr)
        {
            ADD_FAILURE() << "no plot to hover";
            return {};
        }
        const ImRect rect = context.Plots.GetByIndex(0)->PlotRect;
        ImGui::GetIO().AddMousePosEvent(rect.Max.x - 2.0F, (rect.Min.y + rect.Max.y) * 0.5F);
        runFrame(section);
        const std::string captured = renderAndCapture(section);
        const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
        if (tooltip == nullptr || !tooltip->Active)
        {
            ADD_FAILURE() << "no tooltip shown";
            return {};
        }
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start <= captured.size())
        {
            const std::size_t end = std::min(captured.find('\n', start), captured.size());
            const std::string line = captured.substr(start, end - start);
            const std::size_t first = line.find_first_not_of(" \t\r");
            if (first != std::string::npos)
            {
                lines.push_back(line.substr(first, line.find_last_not_of(" \t\r") - first + 1));
            }
            start = end + 1;
        }
        return lines;
    }

  private:
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

// ========== Memory & Swap ==========

TEST(MemorySectionSmoothingTest, FirstSampleStartsAtTheClampedTarget)
{
    MemorySection::SmoothedMemory smoothed;
    Domain::SystemSnapshot snap;
    snap.memoryUsedPercent = 130.0; // a probe glitch above 100% is clamped, not charted
    snap.memoryCachedPercent = -4.0;
    snap.swapUsedPercent = 12.5;

    MemorySection::updateSmoothedMemory(smoothed, snap, 0.016F, REFRESH);

    EXPECT_TRUE(smoothed.initialized);
    EXPECT_DOUBLE_EQ(smoothed.usedPercent, 100.0);
    EXPECT_DOUBLE_EQ(smoothed.cachedPercent, 0.0);
    EXPECT_DOUBLE_EQ(smoothed.swapPercent, 12.5);
}

TEST(MemorySectionSmoothingTest, LaterSamplesEaseByTheFrameAlpha)
{
    MemorySection::SmoothedMemory smoothed;
    Domain::SystemSnapshot snap;
    snap.memoryUsedPercent = 20.0;
    snap.memoryCachedPercent = 10.0;
    snap.swapUsedPercent = 0.0;
    MemorySection::updateSmoothedMemory(smoothed, snap, 0.016F, REFRESH);

    snap.memoryUsedPercent = 80.0;
    snap.memoryCachedPercent = 40.0;
    snap.swapUsedPercent = 50.0;
    constexpr float DELTA = 0.1F;
    MemorySection::updateSmoothedMemory(smoothed, snap, DELTA, REFRESH);

    const double alpha = UI::Widgets::computeAlpha(DELTA, REFRESH);
    ASSERT_GT(alpha, 0.0);
    ASSERT_LT(alpha, 1.0);
    EXPECT_DOUBLE_EQ(smoothed.usedPercent, 20.0 + (alpha * 60.0));
    EXPECT_DOUBLE_EQ(smoothed.cachedPercent, 10.0 + (alpha * 30.0));
    EXPECT_DOUBLE_EQ(smoothed.swapPercent, alpha * 50.0);
}

/// A publication whose memory series are all charted.
[[nodiscard]] Domain::SystemPublication memoryPublication(bool withSwap = true)
{
    Domain::SystemPublication publication;
    publication.timestamps = viewOf(timestampsToNow());
    publication.memoryHistory = viewOf(constant(40.0F));
    publication.memoryCachedHistory = viewOf(constant(15.0F));
    if (withSwap)
    {
        publication.swapHistory = viewOf(constant(5.0F));
    }
    publication.snapshot.memoryTotalBytes = 16ULL << 30U;
    publication.snapshot.memoryUsedBytes = 6ULL << 30U;
    publication.snapshot.memoryUsedPercent = 40.0;
    publication.snapshot.memoryCachedPercent = 15.0;
    publication.snapshot.swapUsedPercent = 5.0;
    return publication;
}

TEST_F(SystemSectionsRenderTest, MemorySectionWithoutAPublicationDrawsNothing)
{
    MemorySection::RenderContext ctx;
    const std::string text =
        renderAndCapture([&] { MemorySection::renderMemorySection(ctx, {}, UI::Widgets::historyFrameNowSeconds(), 3); });
    EXPECT_TRUE(plotsDrawn().empty());
    EXPECT_EQ(text.find("Memory & Swap"), std::string::npos) << text; // not even the heading
}

TEST_F(SystemSectionsRenderTest, MemorySectionDrawsUsedCachedSwapAndPeak)
{
    const Domain::SystemPublication publication = memoryPublication();
    MemorySection::SmoothedMemory smoothed;
    MemorySection::RenderContext ctx{.publication = &publication, .smoothedMemory = &smoothed};
    MemorySection::updateSmoothedMemory(smoothed, publication.snapshot, 0.0F, REFRESH);
    const auto draw = [&]
    {
        runFrame([&] { MemorySection::renderMemorySection(ctx, publication.timestamps, UI::Widgets::historyFrameNowSeconds(), 3); });
    };
    draw();
    draw();

    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 1U);
    EXPECT_EQ(plots[0], (SeriesLabels{"Used", "Cached", "Swap", "Peak Used"}));
    // The heading, and the value strip listing every series with its current value.
    const std::string text = renderAndCapture(
        [&] { MemorySection::renderMemorySection(ctx, publication.timestamps, UI::Widgets::historyFrameNowSeconds(), 3); });
    EXPECT_NE(text.find("Memory & Swap"), std::string::npos) << text;
    EXPECT_NE(text.find("Peak Used"), std::string::npos) << text;
    EXPECT_NE(text.find(UI::Format::formatPercent(40.0)), std::string::npos) << text;
    // The tooltip formats Used and Cached as bytes of the known RAM total, back-calculated from the
    // hovered sample's percent (40% and 15% of 16 GiB), not the percent alone.
    const std::vector<std::string> lines = hoverFirstPlotAndCaptureLines(
        [&] { MemorySection::renderMemorySection(ctx, publication.timestamps, UI::Widgets::historyFrameNowSeconds(), 3); });
    const std::uint64_t total = publication.snapshot.memoryTotalBytes;
    const std::string usedRow = UI::Widgets::formatTooltipRow(
        "Used", UI::Format::bytesUsedTotalPercentCompact(static_cast<std::uint64_t>(0.40 * static_cast<double>(total)), total, 40.0));
    const std::string cachedRow = UI::Widgets::formatTooltipRow(
        "Cached", UI::Format::bytesUsedTotalPercentCompact(static_cast<std::uint64_t>(0.15 * static_cast<double>(total)), total, 15.0));
    EXPECT_TRUE(hasLine(lines, usedRow));
    EXPECT_TRUE(hasLine(lines, cachedRow));
    EXPECT_FALSE(hasLine(lines, UI::Widgets::formatTooltipRow("Used", UI::Format::formatPercent(40.0))));
    EXPECT_FALSE(hasLine(lines, UI::Widgets::formatTooltipRow("Cached", UI::Format::formatPercent(15.0))));
}

TEST_F(SystemSectionsRenderTest, MemorySectionLeavesOutSeriesWithNoHistory)
{
    // No swap on this machine, and an idle chart (0% used): no Swap series and no peak line.
    Domain::SystemPublication publication = memoryPublication(false);
    publication.memoryHistory = viewOf(constant(0.0F));
    publication.snapshot.memoryTotalBytes = 0; // percent-only tooltip rows
    MemorySection::RenderContext ctx{.publication = &publication};
    const auto draw = [&]
    {
        runFrame([&] { MemorySection::renderMemorySection(ctx, publication.timestamps, UI::Widgets::historyFrameNowSeconds(), 3); });
    };
    draw();
    draw();

    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 1U);
    EXPECT_EQ(plots[0], (SeriesLabels{"Used", "Cached"}));
    const std::string text = renderAndCapture(
        [&] { MemorySection::renderMemorySection(ctx, publication.timestamps, UI::Widgets::historyFrameNowSeconds(), 3); });
    EXPECT_EQ(text.find("Peak Used"), std::string::npos) << text; // no peak line, so no strip entry
    // With no RAM total the tooltip's Used and Cached rows are the hovered sample's percent alone,
    // with no "used / total" bytes.
    const std::vector<std::string> lines = hoverFirstPlotAndCaptureLines(
        [&] { MemorySection::renderMemorySection(ctx, publication.timestamps, UI::Widgets::historyFrameNowSeconds(), 3); });
    const std::string usedRow = UI::Widgets::formatTooltipRow("Used", UI::Format::formatPercent(0.0));
    const std::string cachedRow = UI::Widgets::formatTooltipRow("Cached", UI::Format::formatPercent(15.0));
    EXPECT_TRUE(hasLine(lines, usedRow));
    EXPECT_TRUE(hasLine(lines, cachedRow));
    for (const std::string& line : lines)
    {
        EXPECT_EQ(line.find(" / "), std::string::npos) << line;
    }
}

// ========== CPU Cores ==========

[[nodiscard]] Domain::SystemSnapshot coresSnapshot(const std::vector<double>& totals)
{
    Domain::SystemSnapshot snap;
    for (const double total : totals)
    {
        snap.cpuPerCore.push_back(Domain::CpuUsage{.totalPercent = total});
    }
    snap.coreCount = static_cast<int>(totals.size());
    snap.cpuModel = "Test CPU";
    return snap;
}

TEST(CpuCoresSmoothingTest, NullSmoothedVectorIsANoOp)
{
    CpuCoresSection::RenderContext ctx;
    const auto snap = coresSnapshot({50.0});
    CpuCoresSection::updateSmoothedPerCore(snap, ctx); // must not dereference null
    EXPECT_EQ(ctx.smoothedPerCore, nullptr);
}

TEST(CpuCoresSmoothingTest, NewCoresStartAtTheirClampedValue)
{
    std::vector<double> smoothed;
    CpuCoresSection::RenderContext ctx{.lastDeltaSeconds = 0.1F, .refreshInterval = REFRESH, .smoothedPerCore = &smoothed};

    CpuCoresSection::updateSmoothedPerCore(coresSnapshot({25.0, 140.0}), ctx);

    ASSERT_EQ(smoothed.size(), 2U);
    EXPECT_DOUBLE_EQ(smoothed[0], 25.0);
    EXPECT_DOUBLE_EQ(smoothed[1], 100.0);
}

TEST(CpuCoresSmoothingTest, KnownCoresEaseAndAnAddedCoreStartsAtItsValue)
{
    std::vector<double> smoothed;
    constexpr float DELTA = 0.1F;
    CpuCoresSection::RenderContext ctx{.lastDeltaSeconds = DELTA, .refreshInterval = REFRESH, .smoothedPerCore = &smoothed};
    CpuCoresSection::updateSmoothedPerCore(coresSnapshot({10.0}), ctx);

    CpuCoresSection::updateSmoothedPerCore(coresSnapshot({90.0, 60.0}), ctx);

    const double alpha = UI::Widgets::computeAlpha(DELTA, REFRESH);
    ASSERT_EQ(smoothed.size(), 2U);
    EXPECT_DOUBLE_EQ(smoothed[0], 10.0 + (alpha * 80.0));
    EXPECT_DOUBLE_EQ(smoothed[1], 60.0); // not eased up from 0
}

TEST(CpuCoresSmoothingTest, AnUnreadCoreIsNaNAndRestartsAtItsValueWhenItReturns)
{
    std::vector<double> smoothed;
    CpuCoresSection::RenderContext ctx{.lastDeltaSeconds = 0.1F, .refreshInterval = REFRESH, .smoothedPerCore = &smoothed};
    CpuCoresSection::updateSmoothedPerCore(coresSnapshot({30.0, 30.0}), ctx);

    CpuCoresSection::updateSmoothedPerCore(coresSnapshot({30.0, NOT_A_NUMBER}), ctx); // core 1 offline
    ASSERT_EQ(smoothed.size(), 2U);
    EXPECT_TRUE(std::isnan(smoothed[1]));

    CpuCoresSection::updateSmoothedPerCore(coresSnapshot({30.0, 75.0}), ctx); // back online
    EXPECT_DOUBLE_EQ(smoothed[1], 75.0);
}

TEST(CpuCoresSmoothingTest, NoFrameDeltaSnapsToTheReading)
{
    std::vector<double> smoothed;
    CpuCoresSection::RenderContext ctx{.lastDeltaSeconds = 0.1F, .refreshInterval = REFRESH, .smoothedPerCore = &smoothed};
    CpuCoresSection::updateSmoothedPerCore(coresSnapshot({10.0}), ctx);

    ctx.lastDeltaSeconds = 0.0F; // the first frame after the tab is shown
    CpuCoresSection::updateSmoothedPerCore(coresSnapshot({70.0}), ctx);

    EXPECT_DOUBLE_EQ(smoothed[0], 70.0);
}

/// A publication with one constant history per core in @p totals.
[[nodiscard]] Domain::SystemPublication coresPublication(const std::vector<double>& totals)
{
    Domain::SystemPublication publication;
    publication.snapshot = coresSnapshot(totals);
    publication.timestamps = viewOf(timestampsToNow());
    for (const double total : totals)
    {
        publication.perCoreHistory.push_back(viewOf(constant(static_cast<float>(total))));
    }
    return publication;
}

TEST_F(SystemSectionsRenderTest, CpuCoresWithoutAPublicationShowsTheEmptyState)
{
    CpuCoresSection::RenderContext ctx;
    const std::string text = renderAndCapture([&] { CpuCoresSection::renderCpuCoresSection(ctx); });
    EXPECT_TRUE(plotsDrawn().empty());
    EXPECT_NE(text.find("CPU data unavailable"), std::string::npos) << text;
    EXPECT_NE(text.find("no per-core data to show"), std::string::npos) << text;
}

TEST_F(SystemSectionsRenderTest, CpuCoresWithNoPerCoreDataDrawsNoChart)
{
    Domain::SystemPublication publication;
    publication.snapshot.cpuModel = "Test CPU";
    CpuCoresSection::RenderContext ctx{.publication = &publication};
    const std::string text = renderAndCapture([&] { CpuCoresSection::renderCpuCoresSection(ctx); });
    EXPECT_TRUE(plotsDrawn().empty());
    EXPECT_NE(text.find("Test CPU"), std::string::npos) << text; // the model header still shows
    EXPECT_NE(text.find("No per-core data"), std::string::npos) << text;
}

TEST_F(SystemSectionsRenderTest, CpuCoresDrawsOneChartPerCoreAndSmoothsThem)
{
    const Domain::SystemPublication publication = coresPublication({10.0, 20.0, 30.0, 40.0});
    std::vector<double> smoothed;
    CpuCoresSection::RenderContext ctx{.publication = &publication, .smoothedPerCore = &smoothed};
    const auto draw = [&]
    {
        runFrame([&] { CpuCoresSection::renderCpuCoresSection(ctx); });
    };
    draw();
    draw();

    EXPECT_EQ(plotsDrawn().size(), 4U);
    ASSERT_EQ(smoothed.size(), 4U);
    EXPECT_DOUBLE_EQ(smoothed[3], 40.0);
    // Each cell's label names its core and carries its current value (#1193).
    const std::string text = renderAndCapture([&] { CpuCoresSection::renderCpuCoresSection(ctx); });
    for (const char* core : {"Core 0", "Core 1", "Core 2", "Core 3"})
    {
        EXPECT_NE(text.find(core), std::string::npos) << core << " in: " << text;
    }
    EXPECT_NE(text.find(UI::Format::formatPercent(40.0)), std::string::npos) << text;
    EXPECT_TRUE(hoverFirstPlot(draw));
}

TEST_F(SystemSectionsRenderTest, CpuCoresChartsOnlyTheCoresTheProbeReported)
{
    // Ids 0 and 2 seen; slot 1 never reported (#1262), and core 2 has no current reading.
    Domain::SystemPublication publication = coresPublication({10.0, 0.0, NOT_A_NUMBER});
    publication.snapshot.seenCoreIds = {0, 2};
    CpuCoresSection::RenderContext ctx{.publication = &publication}; // no smoothed values: raw readings
    const std::string text = renderAndCapture([&] { CpuCoresSection::renderCpuCoresSection(ctx); });

    EXPECT_EQ(plotsDrawn().size(), 2U);
    EXPECT_NE(text.find("Core 0"), std::string::npos) << text;
    EXPECT_NE(text.find("Core 2"), std::string::npos) << text;
    EXPECT_EQ(text.find("Core 1"), std::string::npos) << text;
    // Core 2 has no current reading: N/A, not a fake 0% (#1146).
    EXPECT_NE(text.find(UI::Format::formatPercent(NOT_A_NUMBER)), std::string::npos) << text;
}

/// The captured line that names @p core ("Core N"), or empty when none does.
[[nodiscard]] std::string lineNaming(const std::string& text, const std::string& core)
{
    std::size_t start = 0;
    while (start < text.size())
    {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string line = text.substr(start, end - start);
        const std::size_t at = line.find(core);
        // "Core 1" must not match "Core 10".
        if (at != std::string::npos && (at + core.size() == line.size() || line[at + core.size()] < '0' || line[at + core.size()] > '9'))
        {
            return line;
        }
        start = end + 1;
    }
    return {};
}

TEST_F(SystemSectionsRenderTest, CpuCoresMarkPerformanceEfficiencyAndLowPowerCoresOnAHybridCpu)
{
    // Core Ultra style (#1536): two P-cores (class 2), one E-core (class 1), one LP E-core (class 0).
    Domain::SystemPublication publication = coresPublication({10.0, 20.0, 30.0, 40.0});
    publication.snapshot.cpuDetails.efficiencyClassByCoreId = {2, 1, 2, 0};
    CpuCoresSection::RenderContext ctx{.publication = &publication};
    const std::string text = renderAndCapture([&] { CpuCoresSection::renderCpuCoresSection(ctx); });

    const std::string bolt = ICON_FA_BOLT;
    const std::string leaf = ICON_FA_LEAF;
    for (const char* pCore : {"Core 0", "Core 2"})
    {
        const std::string line = lineNaming(text, pCore);
        EXPECT_NE(line.find(bolt), std::string::npos) << pCore << ": " << line;
        EXPECT_EQ(line.find(leaf), std::string::npos) << pCore << ": " << line;
    }
    const std::string eCore = lineNaming(text, "Core 1");
    EXPECT_NE(eCore.find(leaf), std::string::npos) << eCore;
    EXPECT_EQ(eCore.find("LP"), std::string::npos) << eCore;
    EXPECT_EQ(eCore.find(bolt), std::string::npos) << eCore;
    const std::string lpCore = lineNaming(text, "Core 3");
    EXPECT_NE(lpCore.find(leaf + "LP"), std::string::npos) << lpCore;

    // A homogeneous CPU's charts carry no marker at all, and the same static marker cache follows it.
    publication.snapshot.cpuDetails.efficiencyClassByCoreId.clear();
    const std::string plain = renderAndCapture([&] { CpuCoresSection::renderCpuCoresSection(ctx); });
    EXPECT_NE(plain.find("Core 3"), std::string::npos) << plain;
    EXPECT_EQ(plain.find(bolt), std::string::npos) << plain;
    EXPECT_EQ(plain.find(leaf), std::string::npos) << plain;
}

TEST_F(SystemSectionsRenderTest, CpuCoresShowNoMarkerOnAHomogeneousCpu)
{
    Domain::SystemPublication publication = coresPublication({10.0, 20.0});
    publication.snapshot.cpuDetails.efficiencyClassByCoreId = {0, 0}; // One class: not hybrid
    CpuCoresSection::RenderContext ctx{.publication = &publication};
    const std::string text = renderAndCapture([&] { CpuCoresSection::renderCpuCoresSection(ctx); });
    EXPECT_NE(text.find("Core 1"), std::string::npos) << text;
    EXPECT_EQ(text.find(ICON_FA_BOLT), std::string::npos) << text;
    EXPECT_EQ(text.find(ICON_FA_LEAF), std::string::npos) << text;
}

// ========== Disk I/O ==========

TEST(StorageSmoothingTest, MissingPointersAreANoOp)
{
    StorageSection::RenderContext ctx;
    StorageSection::updateSmoothedDiskIO(1.0, 2.0, 0.1F, ctx); // must not dereference null
    EXPECT_EQ(ctx.smoothedReadBytesPerSec, nullptr);
}

TEST(StorageSmoothingTest, FirstSampleStartsAtTheTargetThenEases)
{
    double read = 0.0;
    double write = 0.0;
    bool initialized = false;
    StorageSection::RenderContext ctx{.refreshInterval = REFRESH,
                                      .smoothedReadBytesPerSec = &read,
                                      .smoothedWriteBytesPerSec = &write,
                                      .smoothedInitialized = &initialized};

    StorageSection::updateSmoothedDiskIO(1000.0, 2000.0, 0.1F, ctx);
    EXPECT_TRUE(initialized);
    EXPECT_DOUBLE_EQ(read, 1000.0);
    EXPECT_DOUBLE_EQ(write, 2000.0);

    constexpr float DELTA = 0.1F;
    StorageSection::updateSmoothedDiskIO(3000.0, 0.0, DELTA, ctx);
    const double alpha = UI::Widgets::computeAlpha(DELTA, REFRESH);
    EXPECT_DOUBLE_EQ(read, 1000.0 + (alpha * 2000.0));
    EXPECT_DOUBLE_EQ(write, 2000.0 - (alpha * 2000.0));
}

[[nodiscard]] Domain::PerDiskHistory diskHistory(const std::string& name, double read, double write)
{
    return Domain::PerDiskHistory{
        .deviceName = name, .readBytesPerSec = viewOf(constant(read)), .writeBytesPerSec = viewOf(constant(write))};
}

[[nodiscard]] Domain::DiskSnapshot diskSample(const std::string& name, double read, double write)
{
    Domain::DiskSnapshot disk;
    disk.deviceName = name;
    disk.readBytesPerSec = read;
    disk.writeBytesPerSec = write;
    return disk;
}

TEST_F(SystemSectionsRenderTest, StorageWithoutAPublicationShowsTheEmptyState)
{
    StorageSection::RenderContext ctx;
    const std::string text = renderAndCapture([&] { StorageSection::renderStorageSection(ctx); });
    EXPECT_TRUE(plotsDrawn().empty());
    EXPECT_NE(text.find("Disk data unavailable"), std::string::npos) << text;
    EXPECT_NE(text.find("no disk activity to show"), std::string::npos) << text;
    EXPECT_FLOAT_EQ(StorageSection::diskGridMinimumHeight(nullptr, DISPLAY_WIDTH), 0.0F);
}

TEST_F(SystemSectionsRenderTest, StorageWithOneDiskDrawsTheAggregateChart)
{
    Domain::StoragePublication publication;
    publication.timestamps = viewOf(timestampsToNow());
    publication.totalReadHistory = viewOf(constant(4096.0));
    publication.totalWriteHistory = viewOf(constant(1024.0));
    publication.perDiskHistory.push_back(diskHistory("sda", 4096.0, 1024.0));
    publication.snapshot.totalReadBytesPerSec = 4096.0;
    publication.snapshot.totalWriteBytesPerSec = 1024.0;
    double read = 0.0;
    double write = 0.0;
    bool initialized = false;
    StorageSection::RenderContext ctx{.publication = &publication,
                                      .smoothedReadBytesPerSec = &read,
                                      .smoothedWriteBytesPerSec = &write,
                                      .smoothedInitialized = &initialized};
    const auto draw = [&]
    {
        runFrame([&] { StorageSection::renderStorageSection(ctx); });
    };
    draw();
    draw();

    EXPECT_FALSE(StorageSection::usesDiskGrid(&publication));
    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 1U);
    EXPECT_EQ(plots[0], (SeriesLabels{"Read", "Write"}));
    // Rendering updated the aggregate smoothing it was handed.
    EXPECT_TRUE(initialized);
    EXPECT_DOUBLE_EQ(read, 4096.0);
    EXPECT_DOUBLE_EQ(write, 1024.0);
    const std::string text = renderAndCapture([&] { StorageSection::renderStorageSection(ctx); });
    EXPECT_NE(text.find("Disk I/O History"), std::string::npos) << text;
    EXPECT_EQ(text.find("by Device"), std::string::npos) << text; // not the per-disk grid
    EXPECT_NE(text.find(UI::Format::formatBytesPerSec(4096.0)), std::string::npos) << text;
    EXPECT_TRUE(hoverFirstPlot(draw));
}

TEST_F(SystemSectionsRenderTest, StorageGridDrawsOneCellPerDiskAndTracksTheirSmoothing)
{
    Domain::StoragePublication publication;
    publication.timestamps = viewOf(timestampsToNow());
    publication.perDiskHistory.push_back(diskHistory("nvme0n1", 8192.0, 512.0));
    publication.perDiskHistory.push_back(diskHistory("sdb", 100.0, 200.0));
    // The latest sample lists the disks in another order, and sdb is missing from it (unplugging).
    publication.snapshot.disks.push_back(diskSample("sdc", 1.0, 1.0));
    publication.snapshot.disks.push_back(diskSample("nvme0n1", 8192.0, 512.0));

    std::unordered_map<std::string, StorageSection::SmoothedDiskRates> perDisk;
    perDisk["gone"] = StorageSection::SmoothedDiskRates{.readBytesPerSec = 1.0, .writeBytesPerSec = 1.0, .initialized = true};
    perDisk["sdb"] = StorageSection::SmoothedDiskRates{.readBytesPerSec = 5.0, .writeBytesPerSec = 5.0, .initialized = true};
    StorageSection::RenderContext ctx{.publication = &publication, .refreshInterval = REFRESH, .smoothedPerDisk = &perDisk};
    const std::string text = renderAndCapture([&] { StorageSection::renderStorageSection(ctx); });

    EXPECT_TRUE(StorageSection::usesDiskGrid(&publication));
    EXPECT_EQ(plotsDrawn().size(), 2U);
    // The grid's heading with its disk count, then a cell named for each disk in the history.
    EXPECT_NE(text.find("Disk I/O by Device"), std::string::npos) << text;
    EXPECT_NE(text.find("2 disks"), std::string::npos) << text;
    EXPECT_NE(text.find("nvme0n1"), std::string::npos) << text;
    EXPECT_NE(text.find("sdb"), std::string::npos) << text;
    EXPECT_EQ(text.find("sdc"), std::string::npos) << text; // in the sample but not the history
    // sdb is missing from the latest sample: its bars read N/A, not 0.
    EXPECT_NE(text.find("N/A"), std::string::npos) << text;
    // A disk no longer in the history is forgotten; one missing from the latest sample starts afresh.
    EXPECT_FALSE(perDisk.contains("gone"));
    ASSERT_TRUE(perDisk.contains("sdb"));
    EXPECT_FALSE(perDisk.at("sdb").initialized);
    ASSERT_TRUE(perDisk.contains("nvme0n1"));
    EXPECT_TRUE(perDisk.at("nvme0n1").initialized);
    EXPECT_DOUBLE_EQ(perDisk.at("nvme0n1").readBytesPerSec, 8192.0);
    EXPECT_DOUBLE_EQ(perDisk.at("nvme0n1").writeBytesPerSec, 512.0);
}

TEST_F(SystemSectionsRenderTest, StorageGridReservesAtLeastOneRowOfCells)
{
    Domain::StoragePublication single;
    single.perDiskHistory.push_back(diskHistory("sda", 0.0, 0.0));
    Domain::StoragePublication two = single;
    two.perDiskHistory.push_back(diskHistory("sdb", 0.0, 0.0));

    float singleHeight = -1.0F;
    float gridHeight = 0.0F;
    runFrame(
        [&]
        {
            singleHeight = StorageSection::diskGridMinimumHeight(&single, DISPLAY_WIDTH);
            gridHeight = StorageSection::diskGridMinimumHeight(&two, DISPLAY_WIDTH);
        });

    EXPECT_FLOAT_EQ(singleHeight, 0.0F); // no grid for one disk
    // At least the heading line plus a cell taller than a text line.
    EXPECT_GT(gridHeight, ImGui::GetTextLineHeightWithSpacing() * 3.0F);
}

// ========== Network (and the Disk I/O section it hosts) ==========

[[nodiscard]] Domain::SystemSnapshot::InterfaceSnapshot interfaceSample(const std::string& name, double rx, double tx)
{
    Domain::SystemSnapshot::InterfaceSnapshot iface;
    iface.name = name;
    iface.displayName = name;
    iface.rxBytesPerSec = rx;
    iface.txBytesPerSec = tx;
    iface.isUp = true;
    iface.linkSpeedMbps = 1000;
    iface.isVirtualKnown = true;
    return iface;
}

/// A publication with machine totals and two physical interfaces; only eth0 has
/// its own history.
[[nodiscard]] Domain::SystemPublication networkPublication()
{
    Domain::SystemPublication publication;
    publication.version = 7;
    publication.timestamps = viewOf(timestampsToNow());
    publication.netTxHistory = viewOf(constant(2048.0F));
    publication.netRxHistory = viewOf(constant(8192.0F));
    publication.perInterfaceTxHistory["eth0"] = viewOf(constant(1024.0F));
    publication.perInterfaceRxHistory["eth0"] = viewOf(constant(4096.0F));
    publication.snapshot.netTxBytesPerSec = 2048.0;
    publication.snapshot.netRxBytesPerSec = 8192.0;
    publication.snapshot.networkInterfaces.push_back(interfaceSample("eth0", 4096.0, 1024.0));
    publication.snapshot.networkInterfaces.push_back(interfaceSample("wlan0", 0.0, 0.0));
    return publication;
}

/// Everything the Network tab keeps between frames, wired into one context.
struct NetworkInputs
{
    Domain::SystemPublication publication;
    double sent = 0.0;
    double received = 0.0;
    bool netInitialized = false;
    std::string selected;
    bool showAll = false;
    NetInterfaceUtils::InterfaceNameSet withTraffic;
    NetworkSection::FrameCache cache;
    NetworkSection::RenderContext ctx;

    ~NetworkInputs() = default;
    // ctx points at the members above, so a copy would point at the original's.
    NetworkInputs(const NetworkInputs&) = delete;
    NetworkInputs& operator=(const NetworkInputs&) = delete;
    NetworkInputs(NetworkInputs&&) = delete;
    NetworkInputs& operator=(NetworkInputs&&) = delete;

    // Initialized here, not with a default member initializer: CodeQL's unused-function query does
    // not see calls made from default member initializers (alert 3020 on #1549).
    NetworkInputs() : publication(networkPublication())
    {
        ctx = NetworkSection::RenderContext{
            .systemPublication = &publication,
            .hasNetworkCounters = true,
            .refreshInterval = REFRESH,
            .smoothedNetSentBytesPerSec = &sent,
            .smoothedNetRecvBytesPerSec = &received,
            .smoothedNetInitialized = &netInitialized,
            .selectedNetworkInterface = &selected,
            .showAllInterfaces = &showAll,
            .interfacesWithTraffic = &withTraffic,
            .cache = &cache,
        };
    }
};

TEST_F(SystemSectionsRenderTest, NetworkWithoutCountersStillDrawsTheDiskSection)
{
    Domain::StoragePublication storage;
    storage.timestamps = viewOf(timestampsToNow());
    storage.totalReadHistory = viewOf(constant(1.0));
    storage.totalWriteHistory = viewOf(constant(1.0));
    NetworkSection::RenderContext ctx{.storagePublication = &storage, .hasNetworkCounters = false};
    const std::string text = renderAndCapture([&] { NetworkSection::renderNetworkSection(ctx); });

    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 1U); // the disk chart only
    EXPECT_EQ(plots[0], (SeriesLabels{"Read", "Write"}));
    // The band says why, and the disk section follows it (#1210).
    const auto band = text.find("Network monitoring is not available");
    const auto disk = text.find("Disk I/O History");
    ASSERT_NE(band, std::string::npos) << text;
    ASSERT_NE(disk, std::string::npos) << text;
    EXPECT_LT(band, disk);
}

TEST_F(SystemSectionsRenderTest, NetworkTotalDrawsSentAndReceivedAndFillsTheFrameCache)
{
    NetworkInputs inputs;
    const auto draw = [&]
    {
        runFrame([&] { NetworkSection::renderNetworkSection(inputs.ctx); });
    };
    draw();
    draw();

    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 1U); // no storage publication: no disk chart
    EXPECT_EQ(plots[0], (SeriesLabels{"Sent", "Received"}));
    EXPECT_TRUE(inputs.netInitialized);
    EXPECT_DOUBLE_EQ(inputs.sent, 2048.0);
    EXPECT_DOUBLE_EQ(inputs.received, 8192.0);
    // "Total" plus one selector entry per interface, built for this publication.
    EXPECT_EQ(inputs.cache.publication, &inputs.publication);
    EXPECT_EQ(inputs.cache.version, 7U);
    EXPECT_EQ(inputs.cache.interfaceNames.size(), 3U);
    // eth0 moved traffic, so it is remembered; idle wlan0 is not.
    EXPECT_TRUE(inputs.withTraffic.contains("eth0"));
    EXPECT_FALSE(inputs.withTraffic.contains("wlan0"));
    EXPECT_TRUE(inputs.cache.rowsValid);
    EXPECT_EQ(inputs.cache.statusRowText.size(), inputs.cache.statusRows.size());
    // The status table lists eth0 with its rates.
    const std::string text = renderAndCapture([&] { NetworkSection::renderNetworkSection(inputs.ctx); });
    EXPECT_NE(text.find("Interface Status"), std::string::npos) << text;
    EXPECT_NE(text.find("eth0"), std::string::npos) << text;
    EXPECT_NE(text.find(UI::Format::formatBytesPerSec(4096.0)), std::string::npos) << text;
    EXPECT_TRUE(hoverFirstPlot(draw));
}

TEST_F(SystemSectionsRenderTest, NetworkSelectedInterfaceDrawsItsLinesOverTheTotals)
{
    NetworkInputs inputs;
    inputs.selected = "eth0";
    runFrame([&] { NetworkSection::renderNetworkSection(inputs.ctx); });
    runFrame([&] { NetworkSection::renderNetworkSection(inputs.ctx); });

    ASSERT_EQ(inputs.cache.interfaceNames.size(), 3U);
    const std::string& eth0 = inputs.cache.interfaceNames[1];
    EXPECT_EQ(inputs.cache.labelsName, eth0);
    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 1U);
    EXPECT_EQ(plots[0], (SeriesLabels{eth0 + " Sent", eth0 + " Received", "Sent (Total)", "Received (Total)"}));
    // The bars follow the interface's own rates.
    EXPECT_DOUBLE_EQ(inputs.sent, 1024.0);
    EXPECT_DOUBLE_EQ(inputs.received, 4096.0);
    EXPECT_EQ(inputs.cache.linkTextMbps, 1000U);
    // The selected interface's link speed is shown beside the selector.
    const std::string text = renderAndCapture([&] { NetworkSection::renderNetworkSection(inputs.ctx); });
    ASSERT_FALSE(inputs.cache.linkText.empty());
    EXPECT_NE(text.find(inputs.cache.linkText), std::string::npos) << text;
}

TEST_F(SystemSectionsRenderTest, NetworkInterfaceWithoutHistoryFallsBackToTheTotals)
{
    NetworkInputs inputs;
    inputs.selected = "wlan0";
    runFrame([&] { NetworkSection::renderNetworkSection(inputs.ctx); });

    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 1U);
    EXPECT_EQ(plots[0], (SeriesLabels{"Sent (Total)", "Received (Total)"}));
    EXPECT_NE(inputs.cache.unavailableTitle.find("history unavailable"), std::string::npos);
}

TEST_F(SystemSectionsRenderTest, NetworkSelectionOfAVanishedInterfaceResetsToTotal)
{
    NetworkInputs inputs;
    inputs.selected = "usb0"; // unplugged since it was picked
    inputs.sent = 1.0;
    inputs.received = 1.0;
    inputs.netInitialized = true;
    runFrame([&] { NetworkSection::renderNetworkSection(inputs.ctx); });

    EXPECT_TRUE(inputs.selected.empty());
    // The bars restart at the Total's values rather than easing from the old
    // interface's.
    EXPECT_DOUBLE_EQ(inputs.sent, 2048.0);
    EXPECT_DOUBLE_EQ(inputs.received, 8192.0);
    const auto plots = plotsDrawn();
    ASSERT_EQ(plots.size(), 1U);
    EXPECT_EQ(plots[0], (SeriesLabels{"Sent", "Received"}));
}

TEST_F(SystemSectionsRenderTest, NetworkCacheRebuildsForANewPublicationAndShowAll)
{
    NetworkInputs inputs;
    runFrame([&] { NetworkSection::renderNetworkSection(inputs.ctx); });

    // A new publication with one more (idle, down) interface, and "Show all" on.
    inputs.publication.version = 8;
    auto down = interfaceSample("eth1", 0.0, 0.0);
    down.isUp = false;
    inputs.publication.snapshot.networkInterfaces.push_back(down);
    inputs.showAll = true;
    runFrame([&] { NetworkSection::renderNetworkSection(inputs.ctx); });

    EXPECT_EQ(inputs.cache.version, 8U);
    EXPECT_EQ(inputs.cache.interfaceNames.size(), 4U);
    EXPECT_TRUE(inputs.cache.rowsShowAll);
    EXPECT_EQ(inputs.cache.statusRows.size(), 3U); // every interface listed
    const std::string text = renderAndCapture([&] { NetworkSection::renderNetworkSection(inputs.ctx); });
    EXPECT_NE(text.find("eth1"), std::string::npos) << text; // the down interface is in the table
    EXPECT_NE(text.find("Show all"), std::string::npos) << text;
    // Still counted as hidden by default (down, never seen moving traffic), for the "Show all" label.
    EXPECT_EQ(inputs.cache.hiddenCount, 1U);
}

TEST_F(SystemSectionsRenderTest, NetworkTabSharesItsHeightWithTheDiskGrid)
{
    NetworkInputs inputs;
    Domain::StoragePublication storage;
    storage.timestamps = viewOf(timestampsToNow());
    storage.perDiskHistory.push_back(diskHistory("sda", 10.0, 20.0));
    storage.perDiskHistory.push_back(diskHistory("sdb", 30.0, 40.0));
    UI::Widgets::PlotFillState fillState;
    inputs.ctx.storagePublication = &storage;
    inputs.ctx.fillState = &fillState;
    runFrame([&] { NetworkSection::renderNetworkSection(inputs.ctx); });
    const std::string text = renderAndCapture([&] { NetworkSection::renderNetworkSection(inputs.ctx); });

    EXPECT_EQ(inputs.ctx.fill, nullptr); // the fill scope is closed again
    EXPECT_EQ(plotsDrawn().size(), 3U);  // the network chart and one cell per disk
    // Network first, the disk grid after it (#823).
    const auto network = text.find("Interface Status");
    const auto grid = text.find("Disk I/O by Device");
    ASSERT_NE(network, std::string::npos) << text;
    ASSERT_NE(grid, std::string::npos) << text;
    EXPECT_LT(network, grid);
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

// Network Throughput (the selector, heading and chart) and Interface Status are cards (#1587), the
// chart inside the first; the throughput card settles at one height rather than cycling with the
// fill layout (#1617), and the two fill the tab without overflowing it.
TEST_F(SystemSectionsRenderTest, NetworkSectionsAreCardsThatFillTheTab)
{
    NetworkInputs inputs;
    // One disk, as on most machines: its chart shares the tab's height with the network chart. (With
    // no storage publication at all, the disk section's empty state takes whatever height is left,
    // which the fill layout only approaches over several frames.)
    Domain::StoragePublication storage;
    storage.timestamps = viewOf(timestampsToNow());
    storage.totalReadHistory = viewOf(constant(1.0));
    storage.totalWriteHistory = viewOf(constant(1.0));
    inputs.ctx.storagePublication = &storage;
    UI::Widgets::PlotFillState fillState;
    inputs.ctx.fillState = &fillState;
    for (int frame = 0; frame < 4; ++frame) // The fill layout measures from the previous frame
    {
        runFrame([&] { NetworkSection::renderNetworkSection(inputs.ctx); });
    }

    const ImGuiWindow* throughput = findCard("##NetThroughputCard");
    const ImGuiWindow* status = findCard("##InterfaceStatusCard");
    ASSERT_NE(throughput, nullptr);
    ASSERT_NE(status, nullptr);
    EXPECT_NE(throughput->ChildFlags & ImGuiChildFlags_Borders, 0);
    EXPECT_NE(status->ChildFlags & ImGuiChildFlags_Borders, 0);
    EXPECT_GE(status->Pos.y, throughput->Pos.y + throughput->Size.y); // Below it, not overlapping

    ImPlotContext& context = *ImPlot::GetCurrentContext();
    ASSERT_EQ(context.Plots.GetBufSize(), 2); // Network, then the disk chart
    const ImPlotPlot* plot = context.Plots.GetByIndex(0);
    ASSERT_NE(plot, nullptr);
    EXPECT_TRUE(throughput->Rect().Contains(plot->FrameRect)) << "the chart is drawn outside its card";

    const float settledHeight = throughput->Size.y;
    for (int frame = 0; frame < 12; ++frame)
    {
        runFrame([&] { NetworkSection::renderNetworkSection(inputs.ctx); });
        EXPECT_FLOAT_EQ(throughput->Size.y, settledHeight) << "frame " << frame;
    }
    const ImGuiWindow* tab = ImGui::FindWindowByName("System");
    ASSERT_NE(tab, nullptr);
    EXPECT_LE(tab->ScrollMax.y, 0.0F) << "the cards overflow the tab";
}
} // namespace
} // namespace App
