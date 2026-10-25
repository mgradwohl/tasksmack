/// @file test_NowBarsRender.cpp
/// @brief UI::Widgets::renderHistoryWithNowBars()'s other paths (#1395), headless: each bar's tooltip
/// on hover, no bars (just the chart), the strip's marker queue drained by the time it returns (#2161),
/// RenderMetrics capture (the Ctrl+Shift+M overlay's per-chart samples), a value strip in a row too
/// narrow for it, which falls back to compact labels, the table layout with compact spacing (its bars'
/// tooltips), a history chart drawn with anti-aliasing off, and where the value strip puts each label:
/// on the side of its series' Y axis, for a single-axis and a dual-axis chart (#1882). And idle pacing:
/// the bars' settles and a small axis rescale of an idle machine ask for no more than the idle rate,
/// while a real change does (#2626).

#include "Core/AnimationRequest.h"
#include "Core/FramePacing.h"
#include "UI/ChartWidgets.h"
#include "UI/RenderMetrics.h"
#include "UI/SeriesLabelAlignment.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GImGui->LogBuffer: the text a frame drew
#include <implot.h>

#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace UI::Widgets
{
namespace
{

constexpr float DISPLAY_WIDTH = 800.0F;
constexpr float DISPLAY_HEIGHT = 600.0F;
constexpr float PLOT_HEIGHT = 120.0F;

/// Where a frame of NowBarsRenderTest::stripFrame() put the chart's heading, its plot and its row.
struct StripGeometry
{
    float rowLeft = 0.0F;
    float headingRight = 0.0F;
    float headingTop = 0.0F;
    float headingBottom = 0.0F;
    float chartRight = 0.0F; ///< The chart's right edge: the row less the NowBar column
    float plotTop = 0.0F;
    float measuredEntryWidth = 0.0F; ///< Detail::valueStripEntryWidth() of the measured entry
};

/// The entry stripFrame() measures the width of: its head, tail and whether it is on the right axis.
struct MeasuredEntry
{
    std::string_view head;
    std::string_view tail;
    bool rightAxis = false;
};

class NowBarsRenderTest : public ::testing::Test
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
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        RenderMetrics::get().setEnabled(false);
        ImPlot::DestroyContext(m_ImPlot);
        ImGui::DestroyContext(m_ImGui);
    }

    /// Two bars, as the CPU chart's: Total and User. Plain tooltips: "label: value".
    [[nodiscard]] static std::array<NowBar, 2> twoBars()
    {
        return {
            NowBar{.valueText = "42%", .label = "CPU Total", .tooltipText = {}, .value01 = 0.42, .color = ImVec4(0.2F, 0.6F, 1.0F, 1.0F)},
            NowBar{.valueText = "30%", .label = "CPU User", .tooltipText = {}, .value01 = 0.30, .color = ImVec4(0.4F, 0.8F, 1.0F, 1.0F)}};
    }

    struct Drawn
    {
        bool plotCalled = false;
        std::string text;
        int vertices = 0;
    };

    /// One frame: a window @p width wide holding renderHistoryWithNowBars(), with the text it drew.
    static Drawn frame(std::span<const NowBar> bars, NowBarValues values, float width = DISPLAY_WIDTH, bool compactSpacing = false)
    {
        Drawn drawn;
        ImGui::NewFrame();
        RenderMetrics::get().beginFrame(ImGui::GetFrameCount());
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(width, DISPLAY_HEIGHT));
        ImGui::Begin("Charts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        renderHistoryWithNowBars("TestChart",
                                 PLOT_HEIGHT,
                                 [&drawn]
                                 {
                                     drawn.plotCalled = true;
                                     if (ImPlot::BeginPlot("##TestPlot", ImVec2(-1.0F, PLOT_HEIGHT)))
                                     {
                                         ImPlot::EndPlot();
                                     }
                                 },
                                 bars,
                                 {.compactSpacing = compactSpacing, .values = values});
        drawn.text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
        {
            drawn.vertices += list->VtxBuffer.Size;
        }
        return drawn;
    }

    /// One frame: a heading, then renderHistoryWithNowBars() under @p tableId with its value strip
    /// for @p bars, on a chart whose axes are @p axes. Leaves the frame's draw data for swatchAt().
    static StripGeometry
    stripFrame(const char* tableId, std::span<const NowBar> bars, SeriesLabelAlignment::ChartAxes axes = {}, MeasuredEntry measured = {})
    {
        StripGeometry geometry;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("Charts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        geometry.rowLeft = ImGui::GetCursorScreenPos().x;
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        geometry.chartRight = geometry.rowLeft + rowWidth - nowBarsReservedWidth(bars.size(), 0, false, rowWidth);
        geometry.measuredEntryWidth = Detail::valueStripEntryWidth(measured.head, measured.tail, measured.rightAxis);
        ImGui::TextUnformatted("Heading");
        geometry.headingRight = ImGui::GetItemRectMax().x;
        geometry.headingTop = ImGui::GetItemRectMin().y;
        geometry.headingBottom = ImGui::GetItemRectMax().y;
        renderHistoryWithNowBars(tableId,
                                 PLOT_HEIGHT,
                                 [&geometry]
                                 {
                                     geometry.plotTop = ImGui::GetCursorScreenPos().y;
                                     if (ImPlot::BeginPlot("##StripPlot", ImVec2(-1.0F, PLOT_HEIGHT)))
                                     {
                                         ImPlot::EndPlot();
                                     }
                                 },
                                 bars,
                                 {.axes = axes});
        ImGui::End();
        ImGui::Render();
        return geometry;
    }

    /// The top-left corner of the value-strip swatch drawn in @p color above @p belowY (the plot's top,
    /// so the NowBar in the same colour is left out), from the last frame's draw data; FLT_MAX in both
    /// coordinates if none was drawn.
    [[nodiscard]] static ImVec2 swatchAt(const ImVec4& color, float belowY)
    {
        const ImU32 packed = ImGui::ColorConvertFloat4ToU32(color);
        ImVec2 corner(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
        const ImVector<ImDrawList*>& lists = ImGui::GetDrawData()->CmdLists;
        int listIndex = 0;
        while (listIndex < lists.Size)
        {
            const ImVector<ImDrawVert>& vertices = lists[listIndex]->VtxBuffer;
            int vertexIndex = 0;
            while (vertexIndex < vertices.Size)
            {
                const ImDrawVert& vertex = vertices[vertexIndex];
                if (vertex.col == packed && vertex.pos.y < belowY && vertex.pos.x < corner.x)
                {
                    corner = vertex.pos;
                }
                ++vertexIndex;
            }
            ++listIndex;
        }
        return corner;
    }

  private:
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

TEST_F(NowBarsRenderTest, WithoutBarsItIsJustTheChart)
{
    const Drawn drawn = frame({}, NowBarValues::Strip);
    EXPECT_TRUE(drawn.plotCalled);
}

// #2161: the strip queues a swatch for each bar without its own marker, for the chart's markers to be
// cut into. The bars-only path returned without draining the queue, so its entries (a screen position
// and a view of a label) were left for the next chart drawn anywhere. Every path now drains it.
TEST_F(NowBarsRenderTest, TheStripsMarkerQueueIsEmptyWhenItReturns)
{
    const std::array<NowBar, 2> bars = twoBars(); // no markers of their own: both are queued
    const Drawn drawn = frame(bars, NowBarValues::Strip);
    EXPECT_TRUE(drawn.plotCalled);
    EXPECT_TRUE(Detail::pendingStripMarkers().empty());
}

// #1918: a bar with no reading (N/A) is not drawn as a zero bar. A measured 0 still gets its
// MIN_BAR_FILL_HEIGHT baseline in the series' colour; an unavailable bar has none of its colour at all
// (its track is hatched in the muted text colour instead).
TEST_F(NowBarsRenderTest, AnUnavailableBarIsNotDrawnAsAZeroBar)
{
    const ImVec4 color(0.9F, 0.4F, 0.1F, 1.0F);
    const ImU32 packed = ImGui::ColorConvertFloat4ToU32(color);
    const auto verticesInBarColour = [packed]
    {
        int count = 0;
        for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
        {
            for (const ImDrawVert& vertex : list->VtxBuffer)
            {
                count += (vertex.col == packed) ? 1 : 0;
            }
        }
        return count;
    };

    const std::array<NowBar, 1> zero{NowBar{.valueText = "0.0 B/s", .label = "Sent", .tooltipText = {}, .value01 = 0.0, .color = color}};
    static_cast<void>(frame(zero, NowBarValues::None));
    EXPECT_GT(verticesInBarColour(), 0); // the measured 0's baseline

    const std::array<NowBar, 1> unavailable{NowBar{.valueText = "N/A",
                                                   .label = "Sent",
                                                   .tooltipText = {},
                                                   .value01 = 0.0,
                                                   .color = color,
                                                   .marker = ImPlotMarker_None,
                                                   .available = false}};
    const Drawn drawn = frame(unavailable, NowBarValues::None);
    EXPECT_EQ(verticesInBarColour(), 0);
    EXPECT_GT(drawn.vertices, 0); // the hatched track is still there
}

TEST_F(NowBarsRenderTest, ABarBesideTheChartSaysWhatItIsOnHover)
{
    const std::array<NowBar, 2> bars = twoBars();
    static_cast<void>(frame(bars, NowBarValues::None));
    // The bars sit in the table's right-hand column: sweep its lower half until a bar's tooltip shows,
    // "label: value" (no richer text was given).
    bool shown = false;
    for (int column = 0; column < 40 && !shown; ++column)
    {
        for (int row = 1; row <= 6 && !shown; ++row)
        {
            ImGui::GetIO().AddMousePosEvent(DISPLAY_WIDTH - 4.0F - (4.0F * static_cast<float>(column)), 20.0F * static_cast<float>(row));
            static_cast<void>(frame(bars, NowBarValues::None));
            const std::string text = frame(bars, NowBarValues::None).text;
            shown = text.contains("CPU Total: 42%") || text.contains("CPU User: 30%");
        }
    }
    EXPECT_TRUE(shown);
}

TEST_F(NowBarsRenderTest, RenderMetricsRecordsTheChartAndItsBarsWhenOn)
{
    RenderMetrics::get().setEnabled(true);
    const std::array<NowBar, 2> bars = twoBars();
    static_cast<void>(frame(bars, NowBarValues::None));
    static_cast<void>(frame(bars, NowBarValues::None)); // the first frame's samples are published by the next
    bool barsRecorded = false;
    for (const ChartRenderSample& sample : RenderMetrics::get().lastFrame())
    {
        barsRecorded = barsRecorded || sample.id.contains("TestChart/bars");
    }
    EXPECT_TRUE(barsRecorded);
}

TEST_F(NowBarsRenderTest, ANarrowRowShortensTheStripEntriesInsteadOfDroppingThem)
{
    const std::array<NowBar, 2> bars = twoBars();
    // Wide enough: every entry whole.
    static_cast<void>(frame(bars, NowBarValues::Strip));
    const Drawn wide = frame(bars, NowBarValues::Strip);
    EXPECT_TRUE(wide.text.contains("CPU Total : 42%")) << wide.text;
    EXPECT_TRUE(wide.text.contains("CPU User : 30%")) << wide.text;
    // Too narrow for "CPU Total : 42%": the entry keeps its name and fits the rest with an ellipsis.
    static_cast<void>(frame(bars, NowBarValues::Strip, 140.0F));
    const Drawn narrow = frame(bars, NowBarValues::Strip, 140.0F);
    EXPECT_TRUE(narrow.plotCalled);
    EXPECT_TRUE(narrow.text.contains("CPU Total")) << narrow.text;
    EXPECT_TRUE(narrow.text.contains("\xE2\x80\xA6")) << narrow.text; // U+2026
}

TEST_F(NowBarsRenderTest, ABarBesideTheChartSaysWhatItIsOnHoverInACompactTable)
{
    const std::array<NowBar, 2> bars = twoBars();
    static_cast<void>(frame(bars, NowBarValues::None, DISPLAY_WIDTH, /*compactSpacing=*/true));
    // The bars sit in the table's right-hand column: sweep its lower half until a bar's tooltip shows.
    bool shown = false;
    for (int column = 0; column < 40 && !shown; ++column)
    {
        for (int row = 1; row <= 6 && !shown; ++row)
        {
            ImGui::GetIO().AddMousePosEvent(DISPLAY_WIDTH - 4.0F - (4.0F * static_cast<float>(column)), 20.0F * static_cast<float>(row));
            static_cast<void>(frame(bars, NowBarValues::None, DISPLAY_WIDTH, true));
            const std::string text = frame(bars, NowBarValues::None, DISPLAY_WIDTH, true).text;
            shown = text.contains("CPU Total: 42%") || text.contains("CPU User: 30%");
        }
    }
    EXPECT_TRUE(shown);
}

// #1882: on a chart with one Y axis, its scale on the left, the value strip's labels are left-aligned:
// the first follows the chart's heading on its line, the next after it, all in the chart's left half.
TEST_F(NowBarsRenderTest, ASingleAxisChartsLabelsAreLeftAligned)
{
    const std::array<NowBar, 2> bars = twoBars();
    static_cast<void>(stripFrame("SingleAxisStrip", bars));
    const StripGeometry geometry = stripFrame("SingleAxisStrip", bars);
    const auto total = swatchAt(bars[0].color, geometry.plotTop);
    const auto user = swatchAt(bars[1].color, geometry.plotTop);
    ASSERT_LT(total.x, DISPLAY_WIDTH); // drawn
    ASSERT_LT(user.x, DISPLAY_WIDTH);  // drawn

    const float entryGap = ImGui::GetStyle().ItemSpacing.x * 2.0F;
    EXPECT_NEAR(total.x, geometry.headingRight + entryGap, 1.0F);
    EXPECT_GE(total.y, geometry.headingTop); // on the heading's line
    EXPECT_LT(total.y, geometry.headingBottom);
    EXPECT_GT(user.x, total.x);
    EXPECT_NEAR(user.y, total.y, 0.5F);
    EXPECT_LT(user.x, (geometry.rowLeft + geometry.chartRight) * 0.5F);
}

// #1882: on a dual-axis chart the left axis's series is left-aligned, after the heading, and the
// right axis's series (its label ends in SECONDARY_AXIS_MARK) right-aligned, its entry ending at the
// chart's right edge -- on the same line.
TEST_F(NowBarsRenderTest, ADualAxisChartsLabelsAlignToTheirOwnAxes)
{
    const std::array<NowBar, 2> bars{
        NowBar{.valueText = "512", .label = "Threads", .tooltipText = {}, .value01 = 0.5, .color = ImVec4(0.2F, 0.6F, 1.0F, 1.0F)},
        NowBar{.valueText = "12.0/s",
               .label = "Page Faults \xE2\x86\x92", // " →": SECONDARY_AXIS_MARK
               .tooltipText = {},
               .value01 = 0.1,
               .color = ImVec4(1.0F, 0.6F, 0.2F, 1.0F)}};
    const MeasuredEntry faults{.head = "Page Faults", .tail = "12.0/s", .rightAxis = true};
    static_cast<void>(stripFrame("DualAxisStrip", bars, {}, faults));
    const StripGeometry geometry = stripFrame("DualAxisStrip", bars, {}, faults);
    const auto threads = swatchAt(bars[0].color, geometry.plotTop);
    const auto faultsSwatch = swatchAt(bars[1].color, geometry.plotTop);
    ASSERT_LT(threads.x, DISPLAY_WIDTH);      // drawn
    ASSERT_LT(faultsSwatch.x, DISPLAY_WIDTH); // drawn

    const float entryGap = ImGui::GetStyle().ItemSpacing.x * 2.0F;
    EXPECT_NEAR(threads.x, geometry.headingRight + entryGap, 1.0F);
    EXPECT_NEAR(faultsSwatch.x, geometry.chartRight - geometry.measuredEntryWidth, 1.0F);
    EXPECT_GE(faultsSwatch.y, geometry.headingTop); // both on the heading's line
    EXPECT_LT(faultsSwatch.y, geometry.headingBottom);
    EXPECT_NEAR(faultsSwatch.y, threads.y, 0.5F);
}

// #1882: a chart whose only scale is on the right right-aligns every label, the last ending at the
// chart's right edge.
TEST_F(NowBarsRenderTest, ARightAxisChartsLabelsAreRightAligned)
{
    const std::array<NowBar, 2> bars = twoBars();
    const SeriesLabelAlignment::ChartAxes rightAxis{.primary = SeriesLabelAlignment::AxisSide::Right};
    const MeasuredEntry user{.head = "CPU User", .tail = "30%", .rightAxis = false};
    static_cast<void>(stripFrame("RightAxisStrip", bars, rightAxis, user));
    const StripGeometry geometry = stripFrame("RightAxisStrip", bars, rightAxis, user);
    const auto total = swatchAt(bars[0].color, geometry.plotTop);
    const auto userSwatch = swatchAt(bars[1].color, geometry.plotTop);
    ASSERT_LT(total.x, DISPLAY_WIDTH);      // drawn
    ASSERT_LT(userSwatch.x, DISPLAY_WIDTH); // drawn

    EXPECT_NEAR(userSwatch.x, geometry.chartRight - geometry.measuredEntryWidth, 1.0F);
    EXPECT_LT(total.x, userSwatch.x);
    EXPECT_GT(total.x, (geometry.rowLeft + geometry.chartRight) * 0.5F);
}

TEST_F(NowBarsRenderTest, AChartWithAntiAliasingOffClearsItsLinesFlagsOnlyWhileItDraws)
{
    setChartAntiAliasingEnabled(false);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
    ImGui::Begin("Charts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    const ImDrawList* list = ImGui::GetWindowDrawList();
    const ImDrawListFlags before = list->Flags;
    ImDrawListFlags during = 0;
    bool active = false;
    {
        const HistoryChart chart(percentHistoryConfig("##AntiAliasingOff", -60.0, 0.0));
        active = chart.active();
        during = list->Flags;
    }
    const ImDrawListFlags after = list->Flags;
    ImGui::End();
    ImGui::Render();
    setChartAntiAliasingEnabled(true);

    ASSERT_TRUE(active);
    EXPECT_EQ(during & CHART_ANTI_ALIASING_FLAGS_MASK, 0); // the chart's lines drawn without AA
    EXPECT_EQ(after, before);                              // the window's own flags given back
}

// ========== Idle frame pacing (#2626) ==========
//
// At idle the loop renders at the 20 fps idle rate unless something drawn asks for more
// (Core::FramePacing::computeAnimationRate). These draw what an idle Overview draws and check that
// nothing asks for more than that, while a real change still does.

/// The idle path's frame rate (Application.cpp's IDLE_FRAME_RATE: one frame every 50 ms).
constexpr double IDLE_FPS = 20.0;
constexpr double MAX_FPS = 60.0;
constexpr float IDLE_FRAME_SECONDS = 0.05F;

/// The rate the frames drawn since the last call asked the loop for, as it paces it: 0 is the idle path.
[[nodiscard]] double pacedRate()
{
    return Core::FramePacing::computeAnimationRate(Core::AnimationRequest::consume(), IDLE_FPS, MAX_FPS);
}

TEST_F(NowBarsRenderTest, AnIdleMachinesNowBarSettlesStayOnTheIdlePath)
{
    // Each refresh a bar eases toward its new sample, as the panels smooth it (UI::Widgets::computeAlpha,
    // tau 400 ms at the default 1 s refresh). An idle machine's samples move by a few to 20 percent;
    // at half a pixel a frame those settles asked for 30-60 fps for most of every second, so idle
    // rendered about 34 fps instead of 20 (#2626).
    ImGui::GetIO().DeltaTime = IDLE_FRAME_SECONDS;
    constexpr double TAU_SECONDS = 0.4;
    const double keep = std::exp(-static_cast<double>(IDLE_FRAME_SECONDS) / TAU_SECONDS);
    constexpr std::array<double, 4> IDLE_SAMPLES{0.30, 0.42, 0.25, 0.40};
    constexpr int FRAMES_PER_SAMPLE = 20; // 1 s at the idle rate

    std::array<NowBar, 2> bars = twoBars();
    bars[0].value01 = IDLE_SAMPLES[0];
    bars[1].value01 = 1.0 - IDLE_SAMPLES[0];
    static_cast<void>(frame(bars, NowBarValues::Strip)); // the bars' first frame starts them at rest
    static_cast<void>(Core::AnimationRequest::consume());
    for (const double sample : IDLE_SAMPLES)
    {
        for (int i = 0; i < FRAMES_PER_SAMPLE; ++i)
        {
            bars[0].value01 = sample + ((bars[0].value01 - sample) * keep);
            bars[1].value01 = (1.0 - sample) + ((bars[1].value01 - (1.0 - sample)) * keep);
            static_cast<void>(frame(bars, NowBarValues::Strip));
            ASSERT_DOUBLE_EQ(pacedRate(), 0.0) << "sample " << sample << ", frame " << i;
        }
    }

    // A real change -- over half the bar's height at once -- still eases at more than the idle rate.
    bars[0].value01 = 0.95 + ((bars[0].value01 - 0.95) * keep);
    static_cast<void>(frame(bars, NowBarValues::Strip));
    EXPECT_GT(pacedRate(), IDLE_FPS);
}

TEST_F(NowBarsRenderTest, ASmallAxisRescaleStaysOnTheIdlePathAndALargeOneDoesNot)
{
    // A rate chart's eased upper bound used to ask for the full animation rate until it settled to
    // within 0.2 %, seconds after the visible rescale was over (#2626). It now asks for what each step
    // moves on screen.
    ImGui::GetIO().DeltaTime = IDLE_FRAME_SECONDS;
    const auto drawChart = [](double target)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("Charts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const double bound = easedChartUpperBound(ImGui::GetID("##EasedAxis"), target);
        {
            // 300 s across the window scrolls under 3 px/s: the scroll itself needs no more than idle.
            const HistoryChart chart(rateHistoryConfigWithUpper("##EasedAxis", -300.0, 0.0, formatAxisLocalized, bound));
        }
        ImGui::End();
        ImGui::Render();
    };

    drawChart(100.0); // first drawn: starts at its target
    static_cast<void>(Core::AnimationRequest::consume());
    // A 3 % rescale (a peak a little higher than the last): every eased step is a few pixels.
    for (int i = 0; i < 20; ++i)
    {
        drawChart(103.0);
        ASSERT_DOUBLE_EQ(pacedRate(), 0.0) << "frame " << i;
    }
    // Tripling the scale moves the whole chart: that ease gets more frames than idle.
    drawChart(300.0);
    EXPECT_GT(pacedRate(), IDLE_FPS);
}

} // namespace
} // namespace UI::Widgets
