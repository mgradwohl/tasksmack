/// @file test_CellText.cpp
/// @brief A table cell's text (#914, #1920): the pure fit decision, then the cell drawn headless --
/// whole when it fits, cut short with an ellipsis (never inside a UTF-8 character) when it does not,
/// and the whole text as the clipped cell's tooltip.

#include "UI/CellText.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GImGui->LogBuffer

#include <algorithm>
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

// ---- Cut in the middle, keeping the tail (#1942) ----

/// One pixel per character (UTF-8 code point), the ellipsis included: widths a test can count.
[[nodiscard]] float characterCount(std::string_view text)
{
    float count = 0.0F;
    for (const char byte : text)
    {
        count += ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U) ? 1.0F : 0.0F;
    }
    return count;
}

/// Whether @p text is whole UTF-8 characters: no sequence cut short, no stray continuation byte.
[[nodiscard]] bool isWholeUtf8(std::string_view text)
{
    std::size_t i = 0;
    while (i < text.size())
    {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if ((lead & 0xE0U) == 0xC0U)
        {
            length = 2;
        }
        else if ((lead & 0xF0U) == 0xE0U)
        {
            length = 3;
        }
        else if ((lead & 0xF8U) == 0xF0U)
        {
            length = 4;
        }
        else if ((lead & 0xC0U) == 0x80U)
        {
            return false;
        }
        if (i + length > text.size())
        {
            return false;
        }
        for (std::size_t j = 1; j < length; ++j)
        {
            if ((static_cast<unsigned char>(text[i + j]) & 0xC0U) != 0x80U)
            {
                return false;
            }
        }
        i += length;
    }
    return true;
}

constexpr std::string_view LONG_IPV6 = "[2605:59ca:2160:bf10:6129:8032:850a:4a31]:50747";
constexpr std::string_view IPV6_PORT_SUFFIX = "]:50747";
constexpr std::string_view ELLIPSIS = MIDDLE_ELLIPSIS;

/// "<head>…<tail>": what a cut reads as.
[[nodiscard]] std::string cut(std::string_view head, std::string_view tail)
{
    return std::string(head).append(ELLIPSIS).append(tail);
}

[[nodiscard]] std::string fitByCount(std::string_view text, std::size_t tailBytes, float budget)
{
    return middleEllipsize(text, tailBytes, budget, characterCount);
}

TEST(MiddleEllipsizeTest, TextThatFitsIsUnchanged)
{
    EXPECT_EQ(fitByCount("[::]:22", 3, 7.0F), "[::]:22");
    EXPECT_EQ(fitByCount("10.0.0.5:55026", 6, 40.0F), "10.0.0.5:55026");
    EXPECT_EQ(fitByCount(LONG_IPV6, IPV6_PORT_SUFFIX.size(), characterCount(LONG_IPV6)), LONG_IPV6);
    EXPECT_EQ(fitByCount("", 0, 0.0F), "");
}

TEST(MiddleEllipsizeTest, CutsTheMiddleSharingTheRoomBetweenStartAndEnd)
{
    // The issue's rows: the start of the address, its last groups, and the port.
    EXPECT_EQ(fitByCount(LONG_IPV6, IPV6_PORT_SUFFIX.size(), 26.0F), cut("[2605:59c", "850a:4a31]:50747"));
    EXPECT_EQ(fitByCount(LONG_IPV6, IPV6_PORT_SUFFIX.size(), 9.0F), cut("[", "]:50747"));
}

TEST(MiddleEllipsizeTest, ThePortStaysVisibleAtAnyWidthThatHoldsIt)
{
    const int wanted = 1 + static_cast<int>(IPV6_PORT_SUFFIX.size()); // "…]:50747"
    for (int width = wanted; width < static_cast<int>(characterCount(LONG_IPV6)); ++width)
    {
        const auto budget = static_cast<float>(width);
        const std::string fitted = fitByCount(LONG_IPV6, IPV6_PORT_SUFFIX.size(), budget);
        EXPECT_TRUE(fitted.ends_with(IPV6_PORT_SUFFIX)) << budget << ": " << fitted;
        EXPECT_FLOAT_EQ(characterCount(fitted), budget) << "as much of the address as fits: " << fitted;
        EXPECT_TRUE(LONG_IPV6.starts_with(fitted.substr(0, fitted.find(ELLIPSIS)))) << fitted;
        EXPECT_EQ(fitted.find(ELLIPSIS), fitted.rfind(ELLIPSIS)) << "one cut: " << fitted;
    }
}

TEST(MiddleEllipsizeTest, BelowThatTheEndOfThePortIsKeptLongest)
{
    EXPECT_EQ(fitByCount(LONG_IPV6, IPV6_PORT_SUFFIX.size(), 7.0F), cut("", ":50747"));
    EXPECT_EQ(fitByCount(LONG_IPV6, IPV6_PORT_SUFFIX.size(), 6.0F), cut("", "50747"));
    EXPECT_EQ(fitByCount(LONG_IPV6, IPV6_PORT_SUFFIX.size(), 2.0F), cut("", "7"));
    EXPECT_EQ(fitByCount(LONG_IPV6, IPV6_PORT_SUFFIX.size(), 1.0F), cut("", ""));
    EXPECT_EQ(fitByCount(LONG_IPV6, IPV6_PORT_SUFFIX.size(), 0.0F), cut("", "")) << "never an empty cell";
    EXPECT_EQ(fitByCount(LONG_IPV6, IPV6_PORT_SUFFIX.size(), -5.0F), cut("", ""));
}

TEST(MiddleEllipsizeTest, IPv4KeepsItsPortToo)
{
    constexpr std::string_view ENDPOINT = "192.168.100.200:55026";
    for (int width = 7; width < static_cast<int>(characterCount(ENDPOINT)); ++width)
    {
        const auto budget = static_cast<float>(width);
        const std::string fitted = fitByCount(ENDPOINT, std::string_view(":55026").size(), budget);
        EXPECT_TRUE(fitted.ends_with(":55026")) << budget << ": " << fitted;
        EXPECT_FLOAT_EQ(characterCount(fitted), budget) << fitted;
    }
    EXPECT_EQ(fitByCount(ENDPOINT, 6, 12.0F), cut("192", "00:55026"));
}

TEST(MiddleEllipsizeTest, NeverSplitsAUtf8Character)
{
    // Two-, three- and four-byte characters on both sides of the cut, and a tail that starts inside one.
    const std::string text = "\xC3\xA9\xC3\xA9\xE2\x82\xAC\xE2\x82\xAC\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xC3\xA9:\xE2\x82\xAC\xE2\x82\xAC";
    for (const std::size_t tailBytes : {std::size_t{0}, std::size_t{2}, std::size_t{7}, text.size(), text.size() + 4})
    {
        for (int width = -1; width <= static_cast<int>(characterCount(text)); ++width)
        {
            const auto budget = static_cast<float>(width);
            const std::string fitted = fitByCount(text, tailBytes, budget);
            EXPECT_TRUE(isWholeUtf8(fitted)) << "tail " << tailBytes << ", budget " << budget;
            EXPECT_LE(characterCount(fitted), std::max(budget, 1.0F)) << "tail " << tailBytes << ", budget " << budget;
        }
    }
    // A tail starting inside a character keeps that character whole.
    EXPECT_EQ(fitByCount(text, 2, 2.0F), cut("", "\xE2\x82\xAC")) << "the last 2 bytes are part of a euro sign";
}

TEST(MiddleEllipsizeTest, UsesTheEllipsisItIsGiven)
{
    // The font's own ellipsis character, which cellTextKeepingTail() passes.
    EXPECT_EQ(middleEllipsize("abcdefghij:42", 3, 7.0F, characterCount, "~"), "ab~j:42");
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

TEST_F(CellTextRenderTest, AnEndpointTooWideIsCutInTheMiddleKeepingItsPort)
{
    // #1942: a trailing ellipsis would cut off the port, the part that tells rows apart.
    float avail = 0.0F;
    float drawnWidth = 0.0F;
    std::string logged;
    DrawnCell drawn;
    for (int frame = 0; frame < 2; ++frame)
    {
        logged = runFrame(
            [&]
            {
                avail = ImGui::GetContentRegionAvail().x;
                drawn.result = cellTextKeepingTail(LONG_IPV6, IPV6_PORT_SUFFIX.size());
                drawn.itemRect = GImGui->LastItemData.Rect;
            });
    }
    ASSERT_LT(avail, ImGui::CalcTextSize(LONG_IPV6.data(), LONG_IPV6.data() + LONG_IPV6.size()).x);
    EXPECT_TRUE(drawn.result.clipped);
    EXPECT_FLOAT_EQ(drawn.itemRect.GetWidth(), avail);
    EXPECT_FALSE(logged.contains(LONG_IPV6)) << logged;
    // What it drew: the start of the address, the font's ellipsis, and the port, within the cell.
    const std::size_t port = logged.find(IPV6_PORT_SUFFIX);
    ASSERT_NE(port, std::string::npos) << logged;
    const std::size_t start = logged.find("[2");
    ASSERT_LT(start, port) << logged;
    const std::string shown = logged.substr(start, port + IPV6_PORT_SUFFIX.size() - start);
    static_cast<void>(runFrame([&] { drawnWidth = ImGui::CalcTextSize(shown.c_str()).x; }));
    EXPECT_LE(drawnWidth, avail + CELL_CLIP_TOLERANCE_PX) << shown;

    // On hover, the whole endpoint.
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(drawn.itemRect.GetCenter().x, drawn.itemRect.GetCenter().y);
    io.DeltaTime = 0.25F;
    for (int frame = 0; frame < 6 && !drawn.result.tooltipShown; ++frame)
    {
        logged = runFrame([&] { drawn.result = cellTextKeepingTail(LONG_IPV6, IPV6_PORT_SUFFIX.size()); });
    }
    EXPECT_TRUE(drawn.result.tooltipShown);
    EXPECT_TRUE(logged.contains(LONG_IPV6)) << logged;
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

TEST_F(CellTextRenderTest, AnEndpointThatFitsIsDrawnWhole)
{
    std::string logged;
    CellTextResult result;
    for (int frame = 0; frame < 2; ++frame)
    {
        logged = runFrame([&] { result = cellTextKeepingTail("[::]:22", 3); });
    }
    EXPECT_FALSE(result.clipped);
    EXPECT_TRUE(logged.contains("[::]:22"));
}

} // namespace
} // namespace UI::Widgets
