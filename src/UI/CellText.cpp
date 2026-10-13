#include "CellText.h"

#include <imgui.h>
#include <imgui_internal.h> // ImGuiWindow, ItemSize/ItemAdd, RenderText/RenderTextEllipsis, ImTextCharToUtf8

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace UI::Widgets
{

namespace
{

/// Lays out and draws a cell for @p text (measured as @p textWidthPx): whole when it fits; otherwise
/// @p drawClipped draws its cut-short form in the cell's bounds and, unless @p options say not to,
/// the whole text is the cell's tooltip.
template<typename DrawClipped>
CellTextResult drawCell(std::string_view text, float textWidthPx, const CellTextOptions& options, const DrawClipped& drawClipped)
{
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
    {
        return {};
    }

    const CellTextLayout layout = cellTextLayout(textWidthPx, ImGui::GetContentRegionAvail().x, options.rightAligned);

    // The steps mirror ImGui::TextEx()'s common path: same position, same item size, so a cell that
    // fits is laid out exactly as ImGui::TextUnformatted() would lay it out.
    const char* textBegin = text.data();
    const char* textEnd = text.data() + text.size();
    const ImVec2 textPos(window->DC.CursorPos.x + layout.offsetX, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
    const float height = ImGui::GetFontSize();
    const ImRect bounds(textPos, ImVec2(textPos.x + layout.itemWidth, textPos.y + height));

    // Advance the layout cursor past the alignment offset as well as the text, as SetCursorPosX()
    // followed by a text item did.
    const float layoutWidth = (layout.clipped && options.layoutFullText) ? textWidthPx : layout.itemWidth;
    ImGui::ItemSize(ImVec2(layout.offsetX + layoutWidth, height), 0.0F);
    if (!ImGui::ItemAdd(bounds, 0))
    {
        return {.clipped = layout.clipped};
    }

    if (!layout.clipped)
    {
        ImGui::RenderText(textPos, textBegin, textEnd, false);
        return {};
    }

    drawClipped(window->DrawList, bounds);

    CellTextResult result{.clipped = true};
    if (options.clippedTooltip && ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * CLIPPED_CELL_TOOLTIP_WRAP_EM);
        ImGui::TextUnformatted(textBegin, textEnd);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
        result.tooltipShown = true;
    }
    return result;
}

} // namespace

CellTextResult cellText(std::string_view text, float textWidthPx, const CellTextOptions& options)
{
    return drawCell(text,
                    textWidthPx,
                    options,
                    [&](ImDrawList* drawList, const ImRect& bounds)
                    {
                        // RenderTextEllipsis() cuts the text where the ellipsis fits, a whole character at
                        // a time, so a multi-byte UTF-8 character is never split.
                        const ImVec2 measuredSize(textWidthPx, bounds.GetHeight());
                        ImGui::RenderTextEllipsis(
                            drawList, bounds.Min, bounds.Max, bounds.Max.x, text.data(), text.data() + text.size(), &measuredSize);
                    });
}

CellTextResult cellText(std::string_view text, const CellTextOptions& options)
{
    const float width = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
    return cellText(text, width, options);
}

CellTextResult cellTextKeepingTail(std::string_view text, std::size_t tailBytes, const CellTextOptions& options)
{
    const float width = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
    return drawCell(text,
                    width,
                    options,
                    [&](ImDrawList* drawList, const ImRect& bounds)
                    {
                        // The font's own ellipsis character, the one cellText()'s cut ends in: ImGui bakes
                        // one from dots for a font that has none.
                        std::array<char, 5> ellipsis{};
                        const int ellipsisBytes = ImTextCharToUtf8(ellipsis.data(), ImGui::GetFont()->EllipsisChar);
                        const std::string shown = middleEllipsize(
                            text,
                            tailBytes,
                            bounds.GetWidth(),
                            [](std::string_view s) { return ImGui::CalcTextSize(s.data(), s.data() + s.size()).x; },
                            std::string_view(ellipsis.data(), static_cast<std::size_t>(ellipsisBytes)));
                        // Fitted to the cell, so this draws it whole; it still clips at the cell's edge
                        // when not even the ellipsis fits.
                        ImGui::RenderTextEllipsis(
                            drawList, bounds.Min, bounds.Max, bounds.Max.x, shown.data(), shown.data() + shown.size(), nullptr);
                    });
}

} // namespace UI::Widgets
