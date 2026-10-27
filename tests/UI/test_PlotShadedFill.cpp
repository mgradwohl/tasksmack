/// @file test_PlotShadedFill.cpp
/// @brief Chart fills draw at the alpha their colour carries, applied once (#3166).
///
/// Every filled history chart (CPU, memory, network, disk, GPU, the stacked CPU bands and the
/// sparklines) draws its fill with ImPlot::PlotShaded and a fill colour whose alpha is the theme's
/// (charts.*_fill, "#RRGGBBAA") or, with no theme fill, the line colour at 35 %. ImPlot v1.1 changed
/// how a fill's alpha is applied: until then ImPlotSpec::FillAlpha multiplied a single-colour fill
/// twice, once in BeginItemEx() and again in the shaded renderer's colour getter (epezent/implot#717).
/// TaskSmack never sets FillAlpha, which defaults to 1, and the second multiplication was skipped for
/// an alpha of 1, so the fix leaves its fills as they were: alpha = colour alpha x 1, before and after.
///
/// This renders the fills headless and reads the fill vertices' colours: TaskSmack's own helpers draw
/// exactly the colour they are given, and a FillAlpha (which no chart passes today) is applied once.
/// Nothing here depends on timing or on the platform.

#include "UI/ChartWidgets.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <implot.h>

#include <array>
#include <cstdint>
#include <vector>

namespace UI::Widgets
{
namespace
{

constexpr float DISPLAY_WIDTH = 400.0F;
constexpr float DISPLAY_HEIGHT = 300.0F;
constexpr float PLOT_HEIGHT = 200.0F;

/// A theme fill as the bundled themes write them: arctic-fire-light's cpu_fill, "#1976D24D" (30 %).
constexpr ImU32 THEME_FILL = IM_COL32(0x19, 0x76, 0xD2, 0x4D);

/// A line colour no other vertex of the frame carries (the legend is off).
constexpr ImU32 LINE_COLOR = IM_COL32(255, 0, 255, 255);

/// An opaque fill colour, for FillAlpha to scale.
constexpr ImU32 OPAQUE_FILL = IM_COL32(0, 200, 100, 255);

constexpr ImU32 RGB_MASK = ~(static_cast<ImU32>(0xFFU) << IM_COL32_A_SHIFT);

constexpr std::array<double, 4> XS{0.0, 1.0, 2.0, 3.0};
constexpr std::array<double, 4> LOWER{0.1, 0.2, 0.1, 0.2};
constexpr std::array<double, 4> UPPER{0.6, 0.8, 0.5, 0.7};

[[nodiscard]] std::uint32_t alphaOf(ImU32 colour)
{
    return (colour >> IM_COL32_A_SHIFT) & 0xFFU;
}

class PlotShadedFillTest : public ::testing::Test
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
        ImPlot::DestroyContext(m_ImPlot);
        ImGui::DestroyContext(m_ImGui);
    }

    /// One frame with one plot that @p draw fills; the colour of every vertex whose RGB is @p rgb's.
    template<typename Draw> [[nodiscard]] static std::vector<ImU32> verticesWithRgbOf(ImU32 rgb, const Draw& draw)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("Fill", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        if (ImPlot::BeginPlot("##FillPlot", ImVec2(-1.0F, PLOT_HEIGHT), ImPlotFlags_NoLegend | ImPlotFlags_NoInputs))
        {
            ImPlot::SetupAxesLimits(0.0, 3.0, 0.0, 1.0, ImPlotCond_Always);
            draw();
            ImPlot::EndPlot();
        }
        ImGui::End();
        ImGui::Render();

        std::vector<ImU32> colours;
        for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
        {
            for (const ImDrawVert& vertex : list->VtxBuffer)
            {
                if ((vertex.col & RGB_MASK) == (rgb & RGB_MASK))
                {
                    colours.push_back(vertex.col);
                }
            }
        }
        return colours;
    }

  private:
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

TEST_F(PlotShadedFillTest, ShadedBandDrawsTheThemeFillAtItsOwnAlpha)
{
    // The stacked CPU bands (plotShadedBand): every fill vertex is the theme colour, alpha 0x4D (77).
    const ImVec4 fill = ImGui::ColorConvertU32ToFloat4(THEME_FILL);
    const std::vector<ImU32> colours = verticesWithRgbOf(
        THEME_FILL, [&fill] { plotShadedBand("##band", XS.data(), LOWER.data(), UPPER.data(), static_cast<int>(XS.size()), fill); });
    ASSERT_FALSE(colours.empty()) << "the band drew no fill vertices";
    for (const ImU32 colour : colours)
    {
        EXPECT_EQ(colour, THEME_FILL) << "fill alpha " << alphaOf(colour) << ", expected " << alphaOf(THEME_FILL);
    }
}

TEST_F(PlotShadedFillTest, LineFillDrawsTheThemeFillAtItsOwnAlpha)
{
    // A single-series chart's area under its line (plotLineWithFill): the theme fill, alpha unchanged.
    const ImVec4 fill = ImGui::ColorConvertU32ToFloat4(THEME_FILL);
    const ImVec4 line = ImGui::ColorConvertU32ToFloat4(LINE_COLOR);
    const std::vector<ImU32> colours = verticesWithRgbOf(
        THEME_FILL, [&] { plotLineWithFill("##series", XS.data(), UPPER.data(), static_cast<int>(XS.size()), line, fill); });
    ASSERT_FALSE(colours.empty()) << "the series drew no fill vertices";
    for (const ImU32 colour : colours)
    {
        EXPECT_EQ(colour, THEME_FILL) << "fill alpha " << alphaOf(colour) << ", expected " << alphaOf(THEME_FILL);
    }
}

TEST_F(PlotShadedFillTest, FillAlphaIsAppliedOnce)
{
    // No chart passes FillAlpha; if one ever does, it scales the fill once (ImPlot v1.1, #717):
    // 255 x 0.5 = 127.5, which ImGui rounds to 128. Before v1.1 it was applied twice: 255 x 0.25 -> 64.
    const ImVec4 fill = ImGui::ColorConvertU32ToFloat4(OPAQUE_FILL);
    constexpr float FILL_ALPHA = 0.5F;
    const std::vector<ImU32> colours =
        verticesWithRgbOf(OPAQUE_FILL,
                          [&fill]
                          {
                              ImPlot::PlotShaded("##shaded",
                                                 XS.data(),
                                                 LOWER.data(),
                                                 UPPER.data(),
                                                 static_cast<int>(XS.size()),
                                                 {ImPlotProp_FillColor, fill, ImPlotProp_FillAlpha, FILL_ALPHA});
                          });
    ASSERT_FALSE(colours.empty()) << "PlotShaded drew no fill vertices";
    for (const ImU32 colour : colours)
    {
        EXPECT_EQ(alphaOf(colour), 128U);
    }
}

} // namespace
} // namespace UI::Widgets
