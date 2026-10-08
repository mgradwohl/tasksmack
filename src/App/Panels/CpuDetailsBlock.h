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

/// The Overview's CPU Details block (#809): a "CPU Details" heading with the CPU model beside it,
/// then `rows` as label/value column pairs, as many side by side as the width allows
/// (CpuDetailsText::columnLayout()), so a wide window spends less height on it and more on the
/// charts. A value that is not a reading is drawn muted, with its reason on hover. `rowsGeneration`
/// changes whenever `rows` do (never 0), and `measured` holds their widths between frames.
void render(std::string_view cpuModel, std::span<const CpuDetailsText::Row> rows, std::uint64_t rowsGeneration, MeasuredRows& measured);

} // namespace App::CpuDetailsBlock
