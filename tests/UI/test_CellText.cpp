/// @file test_CellText.cpp
/// @brief A table cell's text (#914, #1920): the pure fit decision, then the cell drawn headless --
/// whole when it fits, cut short with an ellipsis (never inside a UTF-8 character) when it does not,
/// and the whole text as the clipped cell's tooltip.

#include "UI/CellText.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GImGui->LogBuffer

#include <cfloat>
#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace UI::Widgets
{
namespace
{

// ---- The pure decision ----

TEST(CellTextLayoutTest, TextThatFitsIsDrawnWhole)
{
    const CellTextLayout layout = cellTextLayout(80.0F, 120.0F, /*rightAligned=*/false);
    EXPECT_FALSE(layout.clipped);
    EXPECT_FLOAT_EQ(layout.itemWidth, 80.0F);
    EXPECT_FLOAT_EQ(layout.offsetX, 0.0F);
}

TEST(CellTextLayoutTest, TextThatFitsCanSitFlushRight)
{
    const CellTextLayout layout = cellTextLayout(80.0F, 120.0F, /*rightAligned=*/true);
    EXPECT_FALSE(layout.clipped);
    EXPECT_FLOAT_EQ(layout.offsetX, 40.0F);
    EXPECT_FLOAT_EQ(layout.itemWidth, 80.0F);
}

TEST(CellTextLayoutTest, TextTooWideFillsTheCellFromItsLeftEdge)
{
    for (const bool rightAligned : {false, true})
    {
        const CellTextLayout layout = cellTextLayout(300.0F, 120.0F, rightAligned);
        EXPECT_TRUE(layout.clipped);
        EXPECT_FLOAT_EQ(layout.itemWidth, 120.0F) << "the ellipsis goes at the cell's edge";
        EXPECT_FLOAT_EQ(layout.offsetX, 0.0F) << "the leading characters identify the value";
    }
}

TEST(CellTextLayoutTest, AHairOverTheEdgeStillFits)
{
    // Text exactly filling its cell can measure a fraction wider than it: no ellipsis for that.
    EXPECT_FALSE(isCellTextClipped(120.0F + CELL_CLIP_TOLERANCE_PX, 120.0F));
    EXPECT_TRUE(isCellTextClipped(120.0F + CELL_CLIP_TOLERANCE_PX + 0.01F, 120.0F));
}

TEST(CellTextLayoutTest, ANegativeRoomClipsToNothing)
{
    // A column squeezed past its padding: clipped, and never a negative width.
    const CellTextLayout layout = cellTextLayout(10.0F, -4.0F, /*rightAligned=*/false);
    EXPECT_TRUE(layout.clipped);
    EXPECT_FLOAT_EQ(layout.itemWidth, 0.0F);
}

TEST(CellTextLayoutTest, NonFiniteWidthsNeverClip)
{
    constexpr float NOT_A_NUMBER = std::numeric_limits<float>::quiet_NaN();
    constexpr float INFINITE = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(isCellTextClipped(NOT_A_NUMBER, 120.0F));
    EXPECT_FALSE(isCellTextClipped(80.0F, NOT_A_NUMBER));
    EXPECT_FALSE(isCellTextClipped(INFINITE, 120.0F));
    const CellTextLayout layout = cellTextLayout(80.0F, NOT_A_NUMBER, /*rightAligned=*/true);
    EXPECT_FALSE(layout.clipped);
    EXPECT_FLOAT_EQ(layout.offsetX, 0.0F);
}

// ---- Drawn headless ----

/// What one cellText() call drew.
struct DrawnCell
{
    CellTextResult result;
    std::vector<float> glyphU0; ///< Each glyph quad's left texture coordinate, in drawing order.
    ImRect itemRect;
    float ellipsisU0 = -1.0F; ///< The font's ellipsis glyph's, for comparison.
};

class CellTextRenderTest : public ::testing::Test
{
  protected:
    static constexpr float WINDOW_WIDTH = 120.0F;

    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(800.0F, 600.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame drawing @p body in a window WINDOW_WIDTH wide; returns every text it drew, tooltips
    /// included.
    static std::string runFrame(const std::function<void()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(WINDOW_WIDTH, 200.0F));
        ImGui::Begin("Cells", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        body();
        std::string logged = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return logged;
    }

    /// Draws @p text as a cell, recording the glyph quads it added to the window's draw list.
    static DrawnCell drawCell(std::string_view text, const CellTextOptions& options = {})
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const int before = drawList->VtxBuffer.Size;
        DrawnCell drawn;
        drawn.result = cellText(text, options);
        drawn.itemRect = GImGui->LastItemData.Rect;
        drawn.ellipsisU0 = glyphU0(ImGui::GetFont()->EllipsisChar);
        // Each glyph is one quad of four vertices, the first at its top-left corner.
        for (int vertex = before; vertex + 3 < drawList->VtxBuffer.Size; vertex += 4)
        {
            drawn.glyphU0.push_back(drawList->VtxBuffer[vertex].uv.x);
        }
        return drawn;
    }

    /// The left texture coordinate of @p codepoint's glyph in the current font, inside a frame (baked
    /// by an earlier one, so its place in the atlas is settled).
    [[nodiscard]] static float glyphU0(ImWchar codepoint)
    {
        const ImFontGlyph* glyph = ImGui::GetFontBaked()->FindGlyphNoFallback(codepoint);
        return glyph != nullptr ? glyph->U0 : -1.0F;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(CellTextRenderTest, TextThatFitsIsDrawnUnchanged)
{
    DrawnCell drawn;
    float a = 0.0F;
    float c = 0.0F;
    float width = 0.0F;
    for (int frame = 0; frame < 2; ++frame) // the first frame bakes the glyphs
    {
        static_cast<void>(runFrame(
            [&]
            {
                drawn = drawCell("abc");
                a = glyphU0('a');
                c = glyphU0('c');
                width = ImGui::CalcTextSize("abc").x;
            }));
    }
    EXPECT_FALSE(drawn.result.clipped);
    EXPECT_FALSE(drawn.result.tooltipShown);
    ASSERT_EQ(drawn.glyphU0.size(), 3U) << "every character, no ellipsis";
    EXPECT_FLOAT_EQ(drawn.glyphU0[0], a);
    EXPECT_FLOAT_EQ(drawn.glyphU0[2], c);
    EXPECT_FLOAT_EQ(drawn.itemRect.GetWidth(), width);
}

TEST_F(CellTextRenderTest, TextTooWideEndsInAnEllipsisAtTheCellEdge)
{
    const std::string text(60, 'x');
    DrawnCell drawn;
    float avail = 0.0F;
    float x = 0.0F;
    for (int frame = 0; frame < 2; ++frame)
    {
        static_cast<void>(runFrame(
            [&]
            {
                avail = ImGui::GetContentRegionAvail().x;
                drawn = drawCell(text);
                x = glyphU0('x');
            }));
    }
    EXPECT_TRUE(drawn.result.clipped);
    ASSERT_GE(drawn.glyphU0.size(), 2U);
    EXPECT_LT(drawn.glyphU0.size(), text.size()) << "cut short";
    EXPECT_FLOAT_EQ(drawn.glyphU0.back(), drawn.ellipsisU0) << "ending in the ellipsis";
    EXPECT_FLOAT_EQ(drawn.glyphU0.front(), x);
    EXPECT_FLOAT_EQ(drawn.itemRect.GetWidth(), avail) << "hoverable up to the cell's edge, no further";
}

TEST_F(CellTextRenderTest, AnEllipsisNeverSplitsAUtf8Character)
{
    // U+00E9 (e acute) is two bytes in UTF-8: a cut between them would draw the font's fallback glyph instead.
    std::string text;
    for (int i = 0; i < 40; ++i)
    {
        text += "\xC3\xA9";
    }
    DrawnCell drawn;
    float eAcute = -1.0F;
    for (int frame = 0; frame < 2; ++frame)
    {
        static_cast<void>(runFrame(
            [&]
            {
                drawn = drawCell(text);
                eAcute = glyphU0(0x00E9);
            }));
    }
    ASSERT_GE(eAcute, 0.0F) << "the default font has U+00E9";
    ASSERT_TRUE(drawn.result.clipped);
    ASSERT_GE(drawn.glyphU0.size(), 2U);
    EXPECT_FLOAT_EQ(drawn.glyphU0.back(), drawn.ellipsisU0);
    for (std::size_t glyph = 0; glyph + 1 < drawn.glyphU0.size(); ++glyph)
    {
        EXPECT_FLOAT_EQ(drawn.glyphU0[glyph], eAcute) << "glyph " << glyph << " is a whole U+00E9";
    }
}

TEST_F(CellTextRenderTest, AClippedCellShowsItsWholeTextOnHover)
{
    const std::string text = "BluetoothUserService_10b0a7c";
    ImRect rect;
    static_cast<void>(runFrame([&] { rect = drawCell(text).itemRect; }));
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(rect.GetCenter().x, rect.GetCenter().y);
    io.DeltaTime = 0.25F; // past the tooltip's hover delay in a few frames
    DrawnCell drawn;
    std::string logged;
    for (int frame = 0; frame < 6 && !drawn.result.tooltipShown; ++frame)
    {
        logged = runFrame([&] { drawn = drawCell(text); });
    }
    EXPECT_TRUE(drawn.result.clipped);
    EXPECT_TRUE(drawn.result.tooltipShown);
    // The cell logs its whole text, and the tooltip logs it again.
    const std::size_t first = logged.find(text);
    ASSERT_NE(first, std::string::npos);
    EXPECT_NE(logged.find(text, first + text.size()), std::string::npos);

    // A caller drawing its own tooltip asks for none.
    for (int frame = 0; frame < 3; ++frame)
    {
        static_cast<void>(runFrame([&] { drawn = drawCell(text, {.clippedTooltip = false}); }));
    }
    EXPECT_TRUE(drawn.result.clipped);
    EXPECT_FALSE(drawn.result.tooltipShown);
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

TEST_F(CellTextRenderTest, TextThatFitsHasNoTooltip)
{
    ImRect rect;
    static_cast<void>(runFrame([&] { rect = drawCell("abc").itemRect; }));
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(rect.GetCenter().x, rect.GetCenter().y);
    io.DeltaTime = 0.25F;
    DrawnCell drawn;
    for (int frame = 0; frame < 6; ++frame)
    {
        static_cast<void>(runFrame([&] { drawn = drawCell("abc"); }));
    }
    EXPECT_FALSE(drawn.result.tooltipShown);
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

TEST_F(CellTextRenderTest, AClippedCellReportsItsWholeWidthToTheLayoutUnlessAskedNotTo)
{
    // As ImGui::TextUnformatted() does, so a table column's "Size to fit" still fits the text; the
    // Processes table keeps its cells' own width.
    const std::string text(60, 'x');
    float fullAdvance = 0.0F;
    float cellAdvance = 0.0F;
    float wholeText = 0.0F;
    static_cast<void>(runFrame(
        [&]
        {
            wholeText = ImGui::CalcTextSize(text.c_str()).x;
            const float start = ImGui::GetCursorScreenPos().x;
            static_cast<void>(cellText(text));
            fullAdvance = GImGui->CurrentWindow->DC.CursorMaxPos.x - start;
        }));
    static_cast<void>(runFrame(
        [&]
        {
            const float start = ImGui::GetCursorScreenPos().x;
            static_cast<void>(cellText(text, {.layoutFullText = false}));
            cellAdvance = GImGui->CurrentWindow->DC.CursorMaxPos.x - start;
        }));
    EXPECT_FLOAT_EQ(fullAdvance, wholeText);
    EXPECT_LT(cellAdvance, wholeText);
}

} // namespace
} // namespace UI::Widgets
