/// @file test_HistoryChartCsvRender.cpp
/// @brief A history chart's "Copy data as CSV" menu (#808), headless: right-click the chart, choose
/// the item, and the next frame's series -- whole, as they were handed to the chart, the real values
/// a stacked chart registers with exportSeries() rather than its drawn tops -- are on the clipboard.

#include "Mocks/FakeClipboard.h"
#include "UI/ChartWidgets.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GImGui->OpenPopupStack, ImHashStr
#include <implot.h>

#include <array>
#include <span>
#include <string>

namespace UI::Widgets
{
namespace
{

constexpr float DISPLAY_WIDTH = 800.0F;
constexpr float DISPLAY_HEIGHT = 600.0F;
constexpr float PLOT_HEIGHT = 200.0F;

class HistoryChartCsvRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_ImGui = ImGui::CreateContext();
        m_ImPlot = ImPlot::CreateContext();
        m_Clipboard.install(); // never the system clipboard, which other processes share (#1890)
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
        Detail::g_CsvRequestPlotId = 0;
        Detail::g_CsvRequestFrame = -1;
        ImPlot::DestroyContext(m_ImPlot);
        ImGui::DestroyContext(m_ImGui);
    }

    /// One frame: a window holding one history chart with three series. "Load" is plain; "Stacked"
    /// is drawn as a cumulative top but registers its own values first; "##Unnamed" has no name.
    static void frame()
    {
        static constexpr std::array<double, 3> X = {-2.0, -1.0, 0.0};
        static constexpr std::array<double, 3> LOAD = {10.0, 20.0, 30.0};
        static constexpr std::array<double, 3> OWN = {1.0, 2.0, 3.0};
        static constexpr std::array<double, 3> TOP = {11.0, 22.0, 33.0}; // LOAD + OWN, as drawn
        static constexpr std::array<double, 3> OTHER = {5.0, 6.0, 7.0};

        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("Charts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        {
            const HistoryChart chart(withHeight(percentHistoryConfig("##CsvChart", -10.0, 0.0), PLOT_HEIGHT));
            if (chart.active())
            {
                plotLineWithFill("Load##load", X.data(), LOAD.data(), 3, ImVec4(1, 1, 1, 1));
                exportSeries("Stacked", std::span<const double>(X), std::span<const double>(OWN));
                plotLineWithFill("Stacked", X.data(), TOP.data(), 3, ImVec4(1, 1, 1, 1));
                plotLineWithFill("##Unnamed", X.data(), OTHER.data(), 3, ImVec4(1, 1, 1, 1));
            }
        }
        ImGui::End();
        ImGui::Render();
    }

    [[nodiscard]] static ImGuiWindow* topPopup()
    {
        return GImGui->OpenPopupStack.empty() ? nullptr : GImGui->OpenPopupStack.back().Window;
    }

    /// Right-clicks the middle of the chart until its menu opens.
    static void openMenu()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(DISPLAY_WIDTH * 0.5F, PLOT_HEIGHT * 0.5F);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        frame();
        frame();
    }

    [[nodiscard]] static std::string clipboard()
    {
        const char* text = ImGui::GetClipboardText();
        return text == nullptr ? std::string{} : std::string(text);
    }

  private:
    TestMocks::FakeClipboard m_Clipboard;
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

TEST_F(HistoryChartCsvRenderTest, CopyDataAsCsvPutsEveryDrawnSeriesOnTheClipboard)
{
    frame();
    openMenu();
    const ImGuiWindow* menu = topPopup();
    ASSERT_NE(menu, nullptr);

    ImGui::ActivateItemByID(ImHashStr("Copy data as CSV", 0, menu->ID));
    frame(); // chosen
    EXPECT_TRUE(clipboard().empty());
    frame(); // the chart draws again, and copies what it draws
    frame();

    const std::string csv = clipboard();
    // Labels without their "##" part; "Stacked" once, with its own values, not its drawn tops.
    EXPECT_TRUE(csv.starts_with("Time (UTC),Seconds ago,Load,Stacked,Value\r\n")) << csv;
    EXPECT_TRUE(csv.contains(",2.000,10,1,5\r\n")) << csv;
    EXPECT_TRUE(csv.contains(",1.000,20,2,6\r\n")) << csv;
    EXPECT_TRUE(csv.contains(",0.000,30,3,7\r\n")) << csv;
    EXPECT_FALSE(csv.contains(",11")) << csv;
}

TEST_F(HistoryChartCsvRenderTest, TheCopyIsTakenOnceAndNothingIsGatheredOtherwise)
{
    frame();
    EXPECT_EQ(Detail::g_CsvCapture, nullptr);
    openMenu();
    ImGui::ActivateItemByID(ImHashStr("Copy data as CSV", 0, topPopup()->ID));
    frame();
    frame();
    ASSERT_FALSE(clipboard().empty());
    EXPECT_EQ(Detail::g_CsvRequestPlotId, 0U); // consumed

    // Later frames don't copy again.
    ImGui::SetClipboardText("");
    frame();
    frame();
    EXPECT_TRUE(clipboard().empty());
    EXPECT_EQ(Detail::g_CsvCapture, nullptr);
}

TEST_F(HistoryChartCsvRenderTest, ARequestIsForItsOwnChartAndOnlyForAFewFrames)
{
    Detail::g_CsvRequestPlotId = 42;
    Detail::g_CsvRequestFrame = 100;
    EXPECT_FALSE(HistoryChart::csvRequestIsFor(42, 100)); // the frame it was asked in: already drawn
    EXPECT_TRUE(HistoryChart::csvRequestIsFor(42, 101));
    EXPECT_TRUE(HistoryChart::csvRequestIsFor(42, 100 + Detail::CSV_REQUEST_FRAMES));
    EXPECT_FALSE(HistoryChart::csvRequestIsFor(42, 101 + Detail::CSV_REQUEST_FRAMES)); // its chart went away
    EXPECT_FALSE(HistoryChart::csvRequestIsFor(7, 101));                               // another chart
    Detail::g_CsvRequestPlotId = 0;
    EXPECT_FALSE(HistoryChart::csvRequestIsFor(0, 101));
}

TEST(HistoryChartCsvLabelTest, HeadersDropTheIdAndTheRightHandAxisMark)
{
    EXPECT_EQ(Detail::csvSeriesLabel("CPU Total##cpu"), "CPU Total");
    EXPECT_EQ(Detail::csvSeriesLabel("Rate →"), "Rate");
    EXPECT_EQ(Detail::csvSeriesLabel("Rate →##y2"), "Rate");
    EXPECT_EQ(Detail::csvSeriesLabel("##Core"), "");
    EXPECT_EQ(Detail::csvSeriesLabel("Plain"), "Plain");
}

} // namespace
} // namespace UI::Widgets
