/// @file test_BatteryDetails.cpp
/// @brief The Overview's battery details (#1523): the wear and capacity text, every row's known
/// and unknown forms with their reasons, and the rows rendered headless, an unknown value's reason
/// shown on hover.

#include "App/Panels/BatteryDetailsText.h"
#include "App/Panels/CpuDetailsBlock.h"
#include "App/Panels/CpuDetailsText.h"
#include "Domain/SystemSnapshot.h"
#include "UI/Format.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <string>
#include <string_view>
#include <vector>

namespace App
{
namespace
{

using BatteryDetailsText::buildRows;
using BatteryDetailsText::wearPercent;
using CpuDetailsText::Row;

[[nodiscard]] Domain::PowerStatus fullyReported()
{
    Domain::PowerStatus power;
    power.hasBattery = true;
    power.reportsCapacity = true;
    power.reportsCycleCount = true;
    power.reportsTechnology = true;
    power.designCapacityWh = 52.6;
    power.fullChargeCapacityWh = 47.34;
    power.cycleCount = 1234;
    power.technology = "Li-ion";
    power.manufacturer = "Contoso";
    power.model = "5B10W51";
    return power;
}

[[nodiscard]] const Row& rowNamed(const std::vector<Row>& rows, std::string_view label)
{
    const auto it = std::ranges::find(rows, label, &Row::label);
    EXPECT_NE(it, rows.end()) << label;
    return *it;
}

TEST(BatteryDetailsTextTest, WearIsTheLostShareOfTheDesignCapacity)
{
    EXPECT_DOUBLE_EQ(wearPercent(45.0, 50.0).value_or(-1.0), 10.0);
    EXPECT_DOUBLE_EQ(wearPercent(52.0, 50.0).value_or(-1.0), 0.0); // Above design: no wear, never negative
    EXPECT_FALSE(wearPercent(45.0, 0.0).has_value());              // No design: no division by zero
    EXPECT_FALSE(wearPercent(0.0, 50.0).has_value());
}

TEST(BatteryDetailsTextTest, EveryFactIsShownWhenReported)
{
    const std::vector<Row> rows = buildRows(fullyReported());
    ASSERT_EQ(rows.size(), 7U);
    EXPECT_EQ(rowNamed(rows, "Design capacity").value, "52.6 Wh");
    EXPECT_EQ(rowNamed(rows, "Full charge").value, "47.3 Wh");
    EXPECT_EQ(rowNamed(rows, "Wear").value, UI::Format::formatPercent(10.0));
    EXPECT_EQ(rowNamed(rows, "Cycles").value, UI::Format::formatIntLocalized(1234U));
    EXPECT_EQ(rowNamed(rows, "Chemistry").value, "Li-ion");
    EXPECT_EQ(rowNamed(rows, "Manufacturer").value, "Contoso");
    EXPECT_EQ(rowNamed(rows, "Model").value, "5B10W51");
    EXPECT_TRUE(std::ranges::all_of(rows, &Row::available));
}

TEST(BatteryDetailsTextTest, UnreportedFactsAreMutedDashesWithAReason)
{
    // A platform that reports none of them (Windows with relative-unit capacities and no counter)
    Domain::PowerStatus power;
    power.hasBattery = true;
    const std::vector<Row> rows = buildRows(power);
    ASSERT_EQ(rows.size(), 7U);
    for (const Row& row : rows)
    {
        EXPECT_FALSE(row.available) << row.label;
        EXPECT_EQ(row.value, CpuDetailsText::UNAVAILABLE_TEXT) << row.label;
        EXPECT_FALSE(row.tooltip.empty()) << row.label;
    }
    EXPECT_EQ(rowNamed(rows, "Cycles").tooltip, BatteryDetailsText::NOT_REPORTED_BY_SYSTEM);
    EXPECT_EQ(rowNamed(rows, "Model").tooltip, BatteryDetailsText::NOT_REPORTED_BY_BATTERY);

    // Reported by the platform but not given by this battery: a different reason
    Domain::PowerStatus partial = fullyReported();
    partial.designCapacityWh = 0.0;
    partial.cycleCount = 0;
    const std::vector<Row> partialRows = buildRows(partial);
    EXPECT_EQ(rowNamed(partialRows, "Design capacity").tooltip, BatteryDetailsText::NOT_REPORTED_BY_BATTERY);
    EXPECT_FALSE(rowNamed(partialRows, "Wear").available);
    EXPECT_EQ(rowNamed(partialRows, "Cycles").tooltip, BatteryDetailsText::NOT_REPORTED_BY_BATTERY);
    EXPECT_TRUE(rowNamed(partialRows, "Full charge").available);
}

class BatteryDetailsRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1600.0F, 1000.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // No renderer: ImGui owns the atlas
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame drawing @p rows; returns the text drawn, tooltips included.
    std::string renderAndCapture(const std::vector<Row>& rows)
    {
        std::string captured;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Overview", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        CpuDetailsBlock::renderRows("##BatteryDetails", rows, 1, m_Measured);
        const ImGuiTable* table = ImGui::TableFindByID(ImGui::GetID("##BatteryDetails"));
        if (table != nullptr)
        {
            // The first row's value cell: the second column's content start, on the first line
            m_FirstValue = ImVec2(table->Columns[1].WorkMinX, table->OuterRect.Min.y);
        }
        captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return captured;
    }

    ImGuiContext* m_Context = nullptr;
    CpuDetailsBlock::MeasuredRows m_Measured;
    ImVec2 m_FirstValue;
};

TEST_F(BatteryDetailsRenderTest, DrawsEveryRowsLabelAndValue)
{
    const std::vector<Row> rows = buildRows(fullyReported());
    const std::string text = renderAndCapture(rows);
    for (const Row& row : rows)
    {
        EXPECT_TRUE(text.contains(row.label)) << row.label << " in:\n" << text;
        EXPECT_TRUE(text.contains(row.value)) << row.value << " in:\n" << text;
    }
    EXPECT_FALSE(text.contains("Serial"));
}

TEST_F(BatteryDetailsRenderTest, HoveringAnUnknownValueShowsWhy)
{
    Domain::PowerStatus power;
    power.hasBattery = true;
    const std::vector<Row> rows = buildRows(power);
    (void) renderAndCapture(rows); // Lays the table out and measures it

    // Inside the dash, half a line down
    ImGui::GetIO().AddMousePosEvent(m_FirstValue.x + (ImGui::GetFontSize() * 0.25F), m_FirstValue.y + (ImGui::GetFontSize() * 0.5F));
    bool shown = false;
    for (int frame = 0; frame < 60 && !shown; ++frame)
    {
        shown = renderAndCapture(rows).contains(rows.front().tooltip);
    }
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    EXPECT_TRUE(shown) << rows.front().tooltip;
}

} // namespace
} // namespace App
