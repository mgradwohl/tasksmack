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

/// The Overview's CPU Details block (#809). Its heading -- a caret, "CPU Details" and, beside it,
/// the CPU model (expanded) or a one-line summary (collapsed) -- is one text line and toggles the
/// block. Expanded, `rows` follow as label/value column pairs across the full width
/// (CpuDetailsText::columnLayout()); collapsed, the heading line is all, no taller than the
/// one-line header it replaced. A value that is not a reading is drawn muted, with its reason on
/// hover. `measured` holds the rows' widths between frames. Returns true when the heading was
/// clicked: the caller flips `expanded` and keeps it.
[[nodiscard]] bool render(const Content& content, MeasuredRows& measured);

/// The rows alone, as render() lays them out, in the table `tableId`: shared with the Battery
/// details beneath the Overview's Battery chart (#1523). `rowsGeneration` changes whenever `rows`
/// do (never 0), so they are measured only then.
void renderRows(const char* tableId, std::span<const CpuDetailsText::Row> rows, std::uint64_t rowsGeneration, MeasuredRows& measured);

} // namespace App::CpuDetailsBlock
