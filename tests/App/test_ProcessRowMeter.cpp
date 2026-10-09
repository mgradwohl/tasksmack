/// @file test_ProcessRowMeter.cpp
/// @brief Inline meters in the Processes table's rows (#1528): the scaling helpers, the per-column
/// toggles, the column maxima, and -- headless -- that a meter is drawn for the rows the clipper
/// submits and for none when it is off.

#include "App/Panels/ProcessRowMeter.h"
#include "App/Panels/ProcessRowMeterView.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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
            m_Snapshots[i].cpuPercent = 50.0;
        }
        m_Maxima.rebuild(m_Snapshots);
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    struct FrameResult
    {
        int rowsSubmitted = 0;
        int metersDrawn = 0;
        int vertices = 0;
    };

    /// One frame of a scrolling, clipped table like the Processes table: a CPU % column whose cells
    /// call renderCellMeter() (when @p callMeter) before their text.
    FrameResult runFrame(const Settings& settings, bool callMeter)
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
    FrameResult settledFrame(const Settings& settings, bool callMeter)
    {
        (void) runFrame(settings, callMeter);
        return runFrame(settings, callMeter);
    }

    static constexpr ProcessRowMeter::Colors COLORS{.low = ImVec4(1.0F, 0.8F, 0.2F, 0.2F), .high = ImVec4(1.0F, 0.2F, 0.2F, 0.5F)};

    ImGuiContext* m_Context = nullptr;
    std::vector<Domain::ProcessSnapshot> m_Snapshots;
    ColumnMaxima m_Maxima;
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
