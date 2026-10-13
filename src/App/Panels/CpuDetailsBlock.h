#pragma once

#include "App/Panels/CpuDetailsText.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace App::CpuDetailsBlock
{

/// Each row's label and value width at the font they were measured with, kept between frames so the
/// block measures its text only when the rows or the font change, not every frame (#1171).
struct MeasuredRows
{
    std::uint64_t rowsGeneration = 0; ///< The rows' generation they were measured for; 0 = never
    float fontSize = 0.0F;            ///< ImGui::GetFontSize() they were measured at
    std::vector<float> labelWidths;   ///< Label column width per row, gap included
    std::vector<float> valueWidths;   ///< Value column width per row, gap included
    /// The rows' column layout (CpuDetailsText::fitColumns(), not yet spread to the width), as laid
    /// out in frame `layoutFrame`: reused for the rest of that frame, so the card's height and the
    /// rows drawn in it come from one decision, and carried to the next frame, whose layout keeps
    /// its pair count unless the width has really changed (CpuDetailsText::ColumnFit, #1919).
    CpuDetailsText::ColumnLayout layout;
    int layoutFrame = -1; ///< ImGui::GetFrameCount() `layout` is for; -1 = never laid out
};

/// What the block shows this frame.
struct Content
{
    std::string_view cpuModel;                 ///< Beside the heading while expanded
    std::string_view collapsedSummary;         ///< Beside the heading while collapsed (CpuDetailsText::collapsedSummary())
    std::span<const CpuDetailsText::Row> rows; ///< The facts, while expanded
    std::uint64_t rowsGeneration = 0;          ///< Changes whenever `rows` do (never 0)
    bool expanded = true;
};

/// Lays `rows` out for a block whose columns must fit in `fitWidthPx` (measuring them first if they
/// or the font changed) and keeps the layout in `measured` for the rest of the frame. Returns the
/// height the rows take when drawn: `rowsPerPair` table rows, each a text line and its cell padding.
float layOutRows(std::span<const CpuDetailsText::Row> rows, std::uint64_t rowsGeneration, MeasuredRows& measured, float fitWidthPx);

/// Begins the card the block is drawn in (UI::Widgets::beginCompactCard()), full width and exactly
/// as tall as the block this frame: its heading line, and its rows laid out (layOutRows()) for the
/// card's width as it is with the tab's scrollbar shown (UI::Widgets::availWidthWithVerticalScrollbar()).
/// Sized from the layout, not ImGuiChildFlags_AutoResizeY, which sized it to last frame's content:
/// a frame that changed the number of rows clipped them, or left a gap under them (#1919). Call
/// render() in it if it is visible, then UI::Widgets::endCard() either way.
[[nodiscard]] bool beginCard(const char* id, const Content& content, MeasuredRows& measured);

/// The Overview's CPU Details block (#809). Its heading -- a caret, "CPU Details" and, beside it,
/// the CPU model (expanded) or a one-line summary (collapsed) -- is one text line and toggles the
/// block. Expanded, `rows` follow as label/value column pairs across the full width
/// (CpuDetailsText::columnLayout()); collapsed, the heading line is all, no taller than the
/// one-line header it replaced. Every row is drawn: the rows are laid out deep enough to hold them
/// all, at the layout beginCard() sized the card for. A value that is not a reading is drawn muted, with its reason on
/// hover. `measured` holds the rows' widths between frames. Returns true when the heading was
/// clicked: the caller flips `expanded` and keeps it.
[[nodiscard]] bool render(const Content& content, MeasuredRows& measured);

/// The rows alone, as render() lays them out, in the table `tableId`: shared with the Battery
/// details beneath the Overview's Battery chart (#1523). `rowsGeneration` changes whenever `rows`
/// do (never 0), so they are measured only then. Laid out (layOutRows()) for the width available
/// with the tab's scrollbar shown, unless layOutRows() already laid them out this frame, and drawn
/// across the full width.
void renderRows(const char* tableId, std::span<const CpuDetailsText::Row> rows, std::uint64_t rowsGeneration, MeasuredRows& measured);

} // namespace App::CpuDetailsBlock
