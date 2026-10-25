/// @file test_PlotLineTexture.cpp
/// @brief ImPlot's anti-aliased lines sample ImGui's baked line texture (#2744).
///
/// Anti-aliased ImPlot lines are textured quads: each segment's two edges carry the UVs of one row
/// of the baked line texture in ImGui's font atlas, and the GPU interpolates across it. ImGui 1.93
/// changed how that row is stored (TexUvLines went from (u0, v, u1, v) to (u0, u1, v, 1/thickness)),
/// and ImPlot v1.0 still read the old layout, so every chart line sampled texels off the baked row:
/// mostly transparent ones, which drew lines faint, speckled or not at all. Chart anti-aliasing is on
/// by default, so every history chart was affected.
///
/// This renders one anti-aliased ImPlot::PlotLine headless with the default font atlas and checks the
/// line's vertices: their UVs are the ones ImGui's own ImDrawList::_SelectLineTexture() picks for the
/// width, they lie inside the baked-lines region of the atlas, and the middle of the line samples an
/// opaque texel. Nothing here depends on timing or on the platform.

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // ImDrawListSharedData::TexUvLines, ImDrawList::_SelectLineTexture()
#include <implot.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace
{

constexpr float DISPLAY_WIDTH = 400.0F;
constexpr float DISPLAY_HEIGHT = 300.0F;
constexpr float PLOT_HEIGHT = 200.0F;

/// The line's colour: no other vertex of the frame carries it (the legend is off).
constexpr ImU32 LINE_COLOR = IM_COL32(255, 0, 255, 255);

/// TexUvLines' length (imgui_internal.h). In ImGui 1.93 each row is (u0, u1, v, 1/thickness).
constexpr std::size_t TEX_UV_LINES_COUNT = IM_DRAWLIST_TEX_LINES_WIDTH_MAX + 1 + IM_DRAWLIST_TEX_LINES_DETAILED_WIDTH_COUNT;

/// Fully opaque in the atlas. The centre of a baked line of width 1 or more is drawn at full alpha.
constexpr int OPAQUE_ALPHA = 255;

/// What one frame drew for the line, and what ImGui itself would use for that width.
struct DrawnLine
{
    std::vector<ImVec2> uvs;   ///< The UV of every vertex the line emitted
    ImVec2 expectedUv0;        ///< _SelectLineTexture()'s UV for one edge of the line
    ImVec2 expectedUv1;        ///< ... and for the other
    ImDrawListFlags flags = 0; ///< The plot window's draw-list flags while the line was drawn
    float uMin = 0.0F;         ///< The baked-lines region of the atlas, in UV space
    float uMax = 0.0F;
    float vMin = 0.0F;
    float vMax = 0.0F;
};

class PlotLineTextureTest : public ::testing::Test
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

    /// One frame with a single ImPlot::PlotLine of @p weight pixels, anti-aliasing as the style sets it.
    static DrawnLine drawLine(float weight)
    {
        DrawnLine drawn;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("Line", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawn.flags = drawList->Flags;

        // ImPlot clamps the weight to at least 1 pixel; ImGui picks the texture row from the same width.
        float fringe = 0.0F;
        drawList->_SelectLineTexture(std::max(1.0F, weight), &drawn.expectedUv0, &drawn.expectedUv1, &fringe, drawList->Flags);

        const ImVec4* texUvLines = drawList->_Data->TexUvLines;
        drawn.uMin = drawn.vMin = 1.0F;
        for (std::size_t i = 0; i < TEX_UV_LINES_COUNT; ++i)
        {
            const ImVec4& row = texUvLines[i];
            drawn.uMin = std::min({drawn.uMin, row.x, row.y});
            drawn.uMax = std::max({drawn.uMax, row.x, row.y});
            drawn.vMin = std::min(drawn.vMin, row.z);
            drawn.vMax = std::max(drawn.vMax, row.z);
        }

        if (ImPlot::BeginPlot("##LinePlot", ImVec2(-1.0F, PLOT_HEIGHT), ImPlotFlags_NoLegend | ImPlotFlags_NoInputs))
        {
            ImPlot::SetupAxesLimits(0.0, 3.0, 0.0, 1.0, ImPlotCond_Always);
            constexpr std::array<double, 4> xs{0.0, 1.0, 2.0, 3.0};
            constexpr std::array<double, 4> ys{0.2, 0.8, 0.3, 0.6};
            ImPlot::PlotLine("##line",
                             xs.data(),
                             ys.data(),
                             static_cast<int>(xs.size()),
                             {ImPlotProp_LineColor, ImGui::ColorConvertU32ToFloat4(LINE_COLOR), ImPlotProp_LineWeight, weight});
            ImPlot::EndPlot();
        }
        ImGui::End();
        ImGui::Render();

        const ImDrawData* drawData = ImGui::GetDrawData();
        for (const ImDrawList* list : drawData->CmdLists)
        {
            for (const ImDrawVert& vertex : list->VtxBuffer)
            {
                if (vertex.col == LINE_COLOR)
                {
                    drawn.uvs.push_back(vertex.uv);
                }
            }
        }
        return drawn;
    }

    /// The atlas's alpha at @p uv (nearest texel), as a GPU sampling the texture would see it.
    [[nodiscard]] static int atlasAlpha(ImVec2 uv)
    {
        ImTextureData* tex = ImGui::GetIO().Fonts->TexData; // GetPixelsAt() is not const
        const int x = std::clamp(static_cast<int>(uv.x * static_cast<float>(tex->Width)), 0, tex->Width - 1);
        const int y = std::clamp(static_cast<int>(uv.y * static_cast<float>(tex->Height)), 0, tex->Height - 1);
        const auto* texel = static_cast<const unsigned char*>(tex->GetPixelsAt(x, y));
        // RGBA32 keeps alpha in its fourth byte; Alpha8 is alpha alone.
        return tex->Format == ImTextureFormat_RGBA32 ? texel[3] : texel[0];
    }

    [[nodiscard]] static bool sameUv(ImVec2 a, ImVec2 b)
    {
        return a.x == b.x && a.y == b.y; // Exact: both are copies of one atlas value
    }

  private:
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

// The widths TaskSmack's charts draw at (1-3 px, and more at high DPI), including a fractional one.
constexpr std::array<float, 6> LINE_WEIGHTS{1.0F, 1.5F, 2.0F, 3.0F, 5.0F, 8.0F};

TEST_F(PlotLineTextureTest, AntiAliasingIsOnByDefault)
{
    // The precondition of the rest: the default style draws lines with the baked line texture.
    const DrawnLine drawn = drawLine(2.0F);
    EXPECT_NE(drawn.flags & ImDrawListFlags_AntiAliasedLines, 0);
    EXPECT_FALSE(drawn.uvs.empty()) << "the line drew no vertices";
}

TEST_F(PlotLineTextureTest, LineUsesImGuisOwnLineTextureUvs)
{
    for (const float weight : LINE_WEIGHTS)
    {
        SCOPED_TRACE(testing::Message() << "line weight " << weight);
        const DrawnLine drawn = drawLine(weight);
        ASSERT_FALSE(drawn.uvs.empty());
        // Report the first offending vertex only: every segment repeats the same two UVs.
        const auto offEdge = std::ranges::find_if(
            drawn.uvs, [&drawn](ImVec2 uv) { return !sameUv(uv, drawn.expectedUv0) && !sameUv(uv, drawn.expectedUv1); });
        EXPECT_TRUE(offEdge == drawn.uvs.end())
            << "vertex UV (" << offEdge->x << ", " << offEdge->y << ") is neither edge of the baked line row: (" << drawn.expectedUv0.x
            << ", " << drawn.expectedUv0.y << ") / (" << drawn.expectedUv1.x << ", " << drawn.expectedUv1.y << ")";
    }
}

TEST_F(PlotLineTextureTest, LineUvsStayInsideTheBakedLinesRegion)
{
    for (const float weight : LINE_WEIGHTS)
    {
        SCOPED_TRACE(testing::Message() << "line weight " << weight);
        const DrawnLine drawn = drawLine(weight);
        ASSERT_FALSE(drawn.uvs.empty());
        const auto outside = std::ranges::find_if(
            drawn.uvs, [&drawn](ImVec2 uv) { return uv.x < drawn.uMin || uv.x > drawn.uMax || uv.y < drawn.vMin || uv.y > drawn.vMax; });
        EXPECT_TRUE(outside == drawn.uvs.end()) << "vertex UV (" << outside->x << ", " << outside->y << ") is outside the baked lines' u ["
                                                << drawn.uMin << ", " << drawn.uMax << "], v [" << drawn.vMin << ", " << drawn.vMax << "]";
    }
}

TEST_F(PlotLineTextureTest, LineCentreSamplesAnOpaqueTexel)
{
    for (const float weight : LINE_WEIGHTS)
    {
        SCOPED_TRACE(testing::Message() << "line weight " << weight);
        const DrawnLine drawn = drawLine(weight);
        ASSERT_FALSE(drawn.uvs.empty());

        // Every segment is a quad whose two edges carry the two UVs; the GPU interpolates between them,
        // so the middle of the line samples the texel halfway across.
        const ImVec2 edge0 = drawn.uvs.front();
        const auto other = std::ranges::find_if(drawn.uvs, [&edge0](ImVec2 uv) { return !sameUv(uv, edge0); });
        ASSERT_NE(other, drawn.uvs.end()) << "the line's vertices all carry one UV";
        const ImVec2 centre((edge0.x + other->x) * 0.5F, (edge0.y + other->y) * 0.5F);
        EXPECT_EQ(atlasAlpha(centre), OPAQUE_ALPHA) << "centre UV (" << centre.x << ", " << centre.y << ")";
    }
}

} // namespace
