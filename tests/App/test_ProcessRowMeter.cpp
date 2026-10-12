/// @file test_ProcessRowMeter.cpp
/// @brief Inline meters in the Processes table's rows (#1528): the scaling helpers, the per-column
/// styles, the column maxima, and -- headless -- that a meter is drawn for the rows the clipper
/// submits and for none when it is off. The sparkline style (#1612) likewise.

#include "App/Panels/ProcessRowMeter.h"
#include "App/Panels/ProcessRowMeterView.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessRowHistory.h"
#include "Domain/ProcessSnapshot.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace App
{
namespace
{

using ProcessRowMeter::absoluteFraction;
using ProcessRowMeter::ColumnMaxima;
using ProcessRowMeter::percentFraction;
using ProcessRowMeter::Scale;
using ProcessRowMeter::scaleOf;
using ProcessRowMeter::Settings;
using ProcessRowMeter::Style;

// ========== Scaling ==========

TEST(ProcessRowMeterTest, PercentIsClampedToTheCell)
{
    EXPECT_FLOAT_EQ(percentFraction(0.0), 0.0F);
    EXPECT_FLOAT_EQ(percentFraction(50.0), 0.5F);
    EXPECT_FLOAT_EQ(percentFraction(100.0), 1.0F);
    EXPECT_FLOAT_EQ(percentFraction(250.0), 1.0F);
    EXPECT_FLOAT_EQ(percentFraction(-3.0), 0.0F);
    EXPECT_FLOAT_EQ(percentFraction(std::numeric_limits<double>::quiet_NaN()), 0.0F);
    EXPECT_FLOAT_EQ(percentFraction(std::numeric_limits<double>::infinity()), 0.0F);
}

TEST(ProcessRowMeterTest, AbsoluteIsAFractionOfTheColumnMax)
{
    EXPECT_FLOAT_EQ(absoluteFraction(25.0, 100.0), 0.25F);
    EXPECT_FLOAT_EQ(absoluteFraction(100.0, 100.0), 1.0F);
    EXPECT_FLOAT_EQ(absoluteFraction(150.0, 100.0), 1.0F); // A value newer than the maxima
    EXPECT_FLOAT_EQ(absoluteFraction(-1.0, 100.0), 0.0F);
}

TEST(ProcessRowMeterTest, AZeroOrUnusableMaxDrawsNothing)
{
    EXPECT_FLOAT_EQ(absoluteFraction(5.0, 0.0), 0.0F);
    EXPECT_FLOAT_EQ(absoluteFraction(0.0, 0.0), 0.0F);
    EXPECT_FLOAT_EQ(absoluteFraction(5.0, -1.0), 0.0F);
    EXPECT_FLOAT_EQ(absoluteFraction(5.0, std::numeric_limits<double>::infinity()), 0.0F);
    EXPECT_FLOAT_EQ(absoluteFraction(std::numeric_limits<double>::quiet_NaN(), 10.0), 0.0F);
}

TEST(ProcessRowMeterTest, OnlyResourceColumnsHaveAMeter)
{
    EXPECT_EQ(scaleOf(ProcessColumn::CpuPercent), Scale::Percent);
    EXPECT_EQ(scaleOf(ProcessColumn::GpuPercent), Scale::Percent);
    EXPECT_EQ(scaleOf(ProcessColumn::Resident), Scale::Absolute);
    EXPECT_EQ(scaleOf(ProcessColumn::NetSent), Scale::Absolute);
    EXPECT_EQ(scaleOf(ProcessColumn::PID), Scale::None);
    EXPECT_EQ(scaleOf(ProcessColumn::Command), Scale::None);
    for (const ProcessColumn col : ProcessRowMeter::METER_COLUMNS)
    {
        EXPECT_NE(scaleOf(col), Scale::None) << getColumnInfo(col).configKey;
    }
}

TEST(ProcessRowMeterTest, AnUnreadValueHasNoMeter)
{
    Domain::ProcessSnapshot proc;
    proc.ioReadBytesPerSec = 1000.0;
    proc.ioAvailable = false;
    proc.gpuUtilPercent = 80.0;
    proc.gpuFieldsRead = false;
    EXPECT_DOUBLE_EQ(ProcessRowMeter::valueOf(proc, ProcessColumn::IoRead), 0.0);
    EXPECT_DOUBLE_EQ(ProcessRowMeter::valueOf(proc, ProcessColumn::GpuPercent), 0.0);
}

// #1799: NPU % and NPU Memory carry a meter and a sparkline as GPU % and GPU Memory do, from the
// process's NPU figures; NPU Engine, a text column, has none.
TEST(ProcessRowMeterTest, NpuPercentAndMemoryHaveMetersFromTheNpuFigures)
{
    EXPECT_EQ(scaleOf(ProcessColumn::NpuPercent), Scale::Percent);
    EXPECT_EQ(scaleOf(ProcessColumn::NpuMemory), Scale::Absolute);
    EXPECT_EQ(scaleOf(ProcessColumn::NpuEngine), Scale::None);
    EXPECT_TRUE(std::ranges::contains(ProcessRowMeter::METER_COLUMNS, ProcessColumn::NpuPercent));
    EXPECT_TRUE(std::ranges::contains(ProcessRowMeter::METER_COLUMNS, ProcessColumn::NpuMemory));
    EXPECT_EQ(ProcessRowMeter::metricOf(ProcessColumn::NpuPercent), Domain::ProcessRowHistory::Metric::NpuPercent);
    EXPECT_EQ(ProcessRowMeter::metricOf(ProcessColumn::NpuMemory), Domain::ProcessRowHistory::Metric::NpuMemory);

    Domain::ProcessSnapshot proc;
    proc.gpuUtilPercent = 10.0;
    proc.gpuMemoryBytes = 1000;
    proc.npuUtilPercent = 40.0;
    proc.npuMemoryBytes = 4096;
    EXPECT_DOUBLE_EQ(ProcessRowMeter::valueOf(proc, ProcessColumn::NpuPercent), 40.0);
    EXPECT_DOUBLE_EQ(ProcessRowMeter::valueOf(proc, ProcessColumn::NpuMemory), 4096.0);
    ColumnMaxima maxima;
    maxima.rebuild({proc});
    EXPECT_FLOAT_EQ(ProcessRowMeter::fractionOf(proc, ProcessColumn::NpuPercent, maxima), 0.4F);
    EXPECT_FLOAT_EQ(ProcessRowMeter::fractionOf(proc, ProcessColumn::NpuMemory, maxima), 1.0F);

    // Read with the GPU fields: unread, no meter.
    proc.gpuFieldsRead = false;
    EXPECT_DOUBLE_EQ(ProcessRowMeter::valueOf(proc, ProcessColumn::NpuPercent), 0.0);
    EXPECT_DOUBLE_EQ(ProcessRowMeter::valueOf(proc, ProcessColumn::NpuMemory), 0.0);

    // Off by default, and a sparkline asks the model to keep the NPU's history.
    Settings settings;
    EXPECT_FALSE(settings.isOn(ProcessColumn::NpuPercent));
    settings.setStyle(ProcessColumn::NpuPercent, Style::Sparkline);
    settings.setStyle(ProcessColumn::NpuMemory, Style::Bar);
    EXPECT_EQ(settings.sparklineMetrics(), Domain::ProcessRowHistory::bitOf(Domain::ProcessRowHistory::Metric::NpuPercent));
    settings.setStyle(ProcessColumn::NpuEngine, Style::Bar);
    EXPECT_FALSE(settings.isOn(ProcessColumn::NpuEngine));
}

// ========== Settings and maxima ==========

TEST(ProcessRowMeterTest, OnlyCpuIsOnByDefault)
{
    const Settings settings;
    for (const ProcessColumn col : allProcessColumns())
    {
        EXPECT_EQ(settings.isOn(col), col == ProcessColumn::CpuPercent) << getColumnInfo(col).configKey;
    }
    EXPECT_TRUE(settings.anyOn());
}

TEST(ProcessRowMeterTest, AColumnWithoutAMeterCannotBeTurnedOn)
{
    Settings settings;
    settings.set(ProcessColumn::Command, true);
    EXPECT_FALSE(settings.isOn(ProcessColumn::Command));
    settings.set(ProcessColumn::CpuPercent, false);
    EXPECT_FALSE(settings.anyOn());
}

TEST(ProcessRowMeterTest, EachColumnIsOffABarOrASparkline)
{
    Settings settings;
    EXPECT_EQ(settings.style(ProcessColumn::CpuPercent), Style::Bar);
    EXPECT_EQ(settings.sparklineMetrics(), 0U); // The default keeps no history
    settings.setStyle(ProcessColumn::CpuPercent, Style::Sparkline);
    settings.setStyle(ProcessColumn::NetSent, Style::Sparkline);
    EXPECT_TRUE(settings.isOn(ProcessColumn::CpuPercent));
    EXPECT_EQ(
        settings.sparklineMetrics(),
        static_cast<Domain::ProcessRowHistory::MetricSet>(Domain::ProcessRowHistory::bitOf(Domain::ProcessRowHistory::Metric::CpuPercent) |
                                                          Domain::ProcessRowHistory::bitOf(Domain::ProcessRowHistory::Metric::NetSent)));
    settings.set(ProcessColumn::NetSent, true); // The booleans' meaning: a bar
    EXPECT_EQ(settings.style(ProcessColumn::NetSent), Style::Bar);
    settings.setStyle(ProcessColumn::Command, Style::Sparkline);
    EXPECT_EQ(settings.style(ProcessColumn::Command), Style::Off);
    settings.setStyle(ProcessColumn::CpuPercent, Style::Off);
    EXPECT_EQ(settings.sparklineMetrics(), 0U);
}

TEST(ProcessRowMeterTest, StylesRoundTripThroughTheirConfigKeys)
{
    for (const Style style : {Style::Off, Style::Bar, Style::Sparkline})
    {
        EXPECT_EQ(ProcessRowMeter::parseStyle(ProcessRowMeter::styleKey(style)), style);
    }
    EXPECT_EQ(ProcessRowMeter::parseStyle("Sparkline"), std::nullopt);
    EXPECT_EQ(ProcessRowMeter::parseStyle(""), std::nullopt);
    EXPECT_EQ(ProcessRowMeter::styleFromLegacyBool(true), Style::Bar);
    EXPECT_EQ(ProcessRowMeter::styleFromLegacyBool(false), Style::Off);
}

TEST(ProcessRowMeterTest, EveryMeterColumnHasAHistoryMetric)
{
    for (const ProcessColumn col : ProcessRowMeter::METER_COLUMNS)
    {
        EXPECT_TRUE(ProcessRowMeter::metricOf(col).has_value()) << getColumnInfo(col).configKey;
    }
    EXPECT_FALSE(ProcessRowMeter::metricOf(ProcessColumn::PID).has_value());
}

TEST(ProcessRowMeterTest, SparklineFractionsScaleLikeTheBars)
{
    Domain::ProcessRowHistory::Ring ring;
    ring.push(0.0F);
    ring.push(50.0F);
    ring.push(200.0F);
    std::array<float, Domain::ProcessRowHistory::RING_CAPACITY> out{};
    // Percent: against 0-100%, clamped
    ASSERT_EQ(ProcessRowMeter::sparklineFractions(ProcessColumn::CpuPercent, ring, out), 3U);
    EXPECT_FLOAT_EQ(out[1], 0.5F);
    EXPECT_FLOAT_EQ(out[2], 1.0F);
    // Absolute: against the ring's own largest value
    ASSERT_EQ(ProcessRowMeter::sparklineFractions(ProcessColumn::IoRead, ring, out), 3U);
    EXPECT_FLOAT_EQ(out[0], 0.0F);
    EXPECT_FLOAT_EQ(out[1], 0.25F);
    EXPECT_FLOAT_EQ(out[2], 1.0F);
}

TEST(ProcessRowMeterTest, MaximaAreTheLargestValueOfEachAbsoluteColumn)
{
    std::vector<Domain::ProcessSnapshot> snapshots(3);
    snapshots[0].memoryBytes = 100;
    snapshots[1].memoryBytes = 400;
    snapshots[2].memoryBytes = 200;
    snapshots[1].netSentBytesPerSec = 5.0;
    snapshots[2].netSentBytesPerSec = std::numeric_limits<double>::quiet_NaN();
    ColumnMaxima maxima;
    maxima.rebuild(snapshots);
    EXPECT_DOUBLE_EQ(maxima.max[toIndex(ProcessColumn::Resident)], 400.0);
    EXPECT_DOUBLE_EQ(maxima.max[toIndex(ProcessColumn::NetSent)], 5.0);
    EXPECT_DOUBLE_EQ(maxima.max[toIndex(ProcessColumn::IoRead)], 0.0);
    EXPECT_FLOAT_EQ(ProcessRowMeter::fractionOf(snapshots[2], ProcessColumn::Resident, maxima), 0.5F);
    EXPECT_FLOAT_EQ(ProcessRowMeter::fractionOf(snapshots[2], ProcessColumn::IoRead, maxima), 0.0F);

    maxima.rebuild({}); // An empty generation
    EXPECT_DOUBLE_EQ(maxima.max[toIndex(ProcessColumn::Resident)], 0.0);
}

// ========== Headless render ==========

class ProcessRowMeterRenderTest : public ::testing::Test
{
  protected:
    static constexpr int ROW_COUNT = 1000;
    static constexpr float TABLE_HEIGHT = 400.0F;

    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1600.0F, 1000.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();

        m_Snapshots.resize(ROW_COUNT);
        for (std::size_t i = 0; i < m_Snapshots.size(); ++i)
        {
            m_Snapshots[i].pid = static_cast<std::int32_t>(i + 1);
            m_Snapshots[i].uniqueKey = i + 1;
            m_Snapshots[i].cpuPercent = 50.0;
        }
        m_Maxima.rebuild(m_Snapshots);
        // Two generations of CPU history for every row: enough for a line.
        const auto cpu = Domain::ProcessRowHistory::bitOf(Domain::ProcessRowHistory::Metric::CpuPercent);
        (void) m_Tracker.advance(m_Snapshots, cpu);
        m_History = m_Tracker.advance(m_Snapshots, cpu);
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    struct FrameResult
    {
        int rowsSubmitted = 0;
        int metersDrawn = 0;
        int sparklinesDrawn = 0;
        int vertices = 0;
    };

    /// One frame of a scrolling, clipped table like the Processes table: a CPU % column whose cells
    /// call renderCellMeter() and renderCellSparkline() (when @p callMeter) before their text.
    FrameResult runFrame(const Settings& settings, bool callMeter, const Domain::ProcessRowHistory::Generation* history = nullptr)
    {
        FrameResult result;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1600.0F, 1000.0F));
        ImGui::Begin("Processes", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::BeginTable("##table", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg, ImVec2(0.0F, TABLE_HEIGHT)))
        {
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 80.0F);
            ImGui::TableSetupColumn("CPU %", ImGuiTableColumnFlags_WidthFixed, 120.0F);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(ROW_COUNT);
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    ++result.rowsSubmitted;
                    const Domain::ProcessSnapshot& proc = m_Snapshots[static_cast<std::size_t>(i)];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", proc.pid);
                    ImGui::TableSetColumnIndex(1);
                    if (callMeter && ProcessRowMeter::renderCellMeter(settings, ProcessColumn::CpuPercent, proc, m_Maxima, COLORS))
                    {
                        ++result.metersDrawn;
                    }
                    if (callMeter && ProcessRowMeter::renderCellSparkline(
                                         settings, ProcessColumn::CpuPercent, history, static_cast<std::size_t>(i), COLORS))
                    {
                        ++result.sparklinesDrawn;
                    }
                    ImGui::TextUnformatted("50.0");
                }
            }
            ImGui::EndTable();
        }
        ImGui::End();
        ImGui::Render();
        result.vertices = ImGui::GetDrawData()->TotalVtxCount;
        return result;
    }

    /// The frame after the clipper has measured the rows.
    FrameResult settledFrame(const Settings& settings, bool callMeter, const Domain::ProcessRowHistory::Generation* history = nullptr)
    {
        (void) runFrame(settings, callMeter, history);
        return runFrame(settings, callMeter, history);
    }

    static Settings sparklineSettings()
    {
        Settings settings;
        settings.setStyle(ProcessColumn::CpuPercent, Style::Sparkline);
        return settings;
    }

    static constexpr ProcessRowMeter::Colors COLORS{.low = ImVec4(1.0F, 0.8F, 0.2F, 0.2F), .high = ImVec4(1.0F, 0.2F, 0.2F, 0.5F)};

    ImGuiContext* m_Context = nullptr;
    std::vector<Domain::ProcessSnapshot> m_Snapshots;
    ColumnMaxima m_Maxima;
    Domain::ProcessRowHistory::Tracker m_Tracker;
    std::shared_ptr<const Domain::ProcessRowHistory::Generation> m_History;
};

TEST_F(ProcessRowMeterRenderTest, MetersAreDrawnForTheVisibleRowsOnly)
{
    const FrameResult frame = settledFrame(Settings{}, true);
    // Every submitted row has a meter, and the clipper submits only what fits in the table
    // (plus its one-row margins), not the 1000 rows behind it.
    EXPECT_GT(frame.metersDrawn, 0);
    EXPECT_EQ(frame.metersDrawn, frame.rowsSubmitted);
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    EXPECT_LE(frame.metersDrawn, static_cast<int>(std::ceil(TABLE_HEIGHT / rowHeight)) + 2);
}

TEST_F(ProcessRowMeterRenderTest, NoMeterIsDrawnWhenItIsOff)
{
    Settings off;
    off.set(ProcessColumn::CpuPercent, false);
    const FrameResult withoutCall = settledFrame(off, false);
    const FrameResult toggledOff = settledFrame(off, true);
    EXPECT_EQ(toggledOff.metersDrawn, 0);
    EXPECT_EQ(toggledOff.vertices, withoutCall.vertices); // Not a single vertex added

    const FrameResult on = settledFrame(Settings{}, true);
    EXPECT_EQ(on.vertices, withoutCall.vertices + (on.metersDrawn * 4)); // One 4-vertex rectangle a row
}

TEST_F(ProcessRowMeterRenderTest, SparklinesAreDrawnForTheVisibleRowsOnly)
{
    const FrameResult frame = settledFrame(sparklineSettings(), true, m_History.get());
    EXPECT_GT(frame.sparklinesDrawn, 0);
    EXPECT_EQ(frame.sparklinesDrawn, frame.rowsSubmitted);
    EXPECT_EQ(frame.metersDrawn, 0); // A sparkline column draws no bar
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    EXPECT_LE(frame.sparklinesDrawn, static_cast<int>(std::ceil(TABLE_HEIGHT / rowHeight)) + 2);
}

TEST_F(ProcessRowMeterRenderTest, NoSparklineWithoutTheStyleOrAHistory)
{
    const FrameResult withoutCall = settledFrame(sparklineSettings(), false);
    // The bar style, a history the model does not keep, and the style with no history: nothing drawn.
    Settings off;
    off.set(ProcessColumn::CpuPercent, false);
    const FrameResult styleOff = settledFrame(off, true, m_History.get());
    const FrameResult noHistory = settledFrame(sparklineSettings(), true, nullptr);
    EXPECT_EQ(styleOff.sparklinesDrawn, 0);
    EXPECT_EQ(styleOff.vertices, withoutCall.vertices);
    EXPECT_EQ(noHistory.sparklinesDrawn, 0);
    EXPECT_EQ(noHistory.vertices, withoutCall.vertices);
    const FrameResult barOnly = settledFrame(Settings{}, true, m_History.get());
    EXPECT_EQ(barOnly.sparklinesDrawn, 0);
    EXPECT_EQ(barOnly.metersDrawn, barOnly.rowsSubmitted);

    // One value is not a line yet.
    Domain::ProcessRowHistory::Tracker fresh;
    const auto oneValue = fresh.advance(m_Snapshots, Domain::ProcessRowHistory::bitOf(Domain::ProcessRowHistory::Metric::CpuPercent));
    const FrameResult young = settledFrame(sparklineSettings(), true, oneValue.get());
    EXPECT_EQ(young.sparklinesDrawn, 0);
    EXPECT_EQ(young.vertices, withoutCall.vertices);
}

TEST_F(ProcessRowMeterRenderTest, TheSparklineEndsAtTheCellsRightEdge)
{
    // Two values, 0% then 100%: one step of the line, rising to the right edge.
    std::vector<Domain::ProcessSnapshot> one(1);
    one[0].uniqueKey = 1;
    Domain::ProcessRowHistory::Tracker tracker;
    const auto cpu = Domain::ProcessRowHistory::bitOf(Domain::ProcessRowHistory::Metric::CpuPercent);
    (void) tracker.advance(one, cpu);
    one[0].cpuPercent = 100.0;
    const auto history = tracker.advance(one, cpu);

    float cellLeft = 0.0F;
    float cellWidth = 0.0F;
    float minX = 0.0F;
    float maxX = 0.0F;
    int added = 0;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(400.0F, 200.0F));
    ImGui::Begin("Cell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::BeginTable("##one", 1))
    {
        ImGui::TableSetupColumn("CPU %", ImGuiTableColumnFlags_WidthFixed, 200.0F);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        cellLeft = ImGui::GetCursorScreenPos().x;
        cellWidth = ImGui::GetContentRegionAvail().x;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const int before = drawList->VtxBuffer.Size;
        EXPECT_TRUE(ProcessRowMeter::renderCellSparkline(sparklineSettings(), ProcessColumn::CpuPercent, history.get(), 0, COLORS));
        added = drawList->VtxBuffer.Size - before;
        if (added > 0)
        {
            minX = drawList->VtxBuffer[before].pos.x;
            maxX = minX;
            int v = before;
            while (v < drawList->VtxBuffer.Size)
            {
                minX = std::min(minX, drawList->VtxBuffer[v].pos.x);
                maxX = std::max(maxX, drawList->VtxBuffer[v].pos.x);
                ++v;
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
    ImGui::Render();
    ASSERT_GT(added, 0);
    const float step = cellWidth / static_cast<float>(Domain::ProcessRowHistory::RING_CAPACITY - 1U);
    const float right = cellLeft + cellWidth;
    // The stroke and its anti-aliasing fringe reach a couple of pixels past the points.
    EXPECT_NEAR(maxX, right, 3.0F);
    EXPECT_NEAR(minX, right - step, 3.0F);
}

TEST_F(ProcessRowMeterRenderTest, TheBarIsProportionalToTheValue)
{
    float barWidth = 0.0F;
    float cellWidth = 0.0F;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(400.0F, 200.0F));
    ImGui::Begin("Cell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::BeginTable("##one", 1))
    {
        ImGui::TableSetupColumn("CPU %", ImGuiTableColumnFlags_WidthFixed, 200.0F);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        cellWidth = ImGui::GetContentRegionAvail().x;
        Domain::ProcessSnapshot proc;
        proc.cpuPercent = 25.0;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const int before = drawList->VtxBuffer.Size;
        EXPECT_TRUE(ProcessRowMeter::renderCellMeter(Settings{}, ProcessColumn::CpuPercent, proc, m_Maxima, COLORS));
        ASSERT_EQ(drawList->VtxBuffer.Size, before + 4);
        barWidth = drawList->VtxBuffer[before + 1].pos.x - drawList->VtxBuffer[before].pos.x;
        ImGui::EndTable();
    }
    ImGui::End();
    ImGui::Render();
    EXPECT_NEAR(barWidth, cellWidth * 0.25F, 0.01F);
}

} // namespace
} // namespace App
