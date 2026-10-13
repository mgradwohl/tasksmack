#include "CellText.h"

#include <imgui.h>
#include <imgui_internal.h> // ImGuiWindow, ItemSize/ItemAdd, RenderText/RenderTextEllipsis

#include <string_view>

namespace UI::Widgets
{

CellTextResult cellText(std::string_view text, float textWidthPx, const CellTextOptions& options)
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

    // RenderTextEllipsis() cuts the text where the ellipsis fits, a whole character at a time, so a
    // multi-byte UTF-8 character is never split.
    const ImVec2 measuredSize(textWidthPx, height);
    ImGui::RenderTextEllipsis(window->DrawList, bounds.Min, bounds.Max, bounds.Max.x, textBegin, textEnd, &measuredSize);

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

CellTextResult cellText(std::string_view text, const CellTextOptions& options)
{
    const float width = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
    return cellText(text, width, options);
}

} // namespace UI::Widgets
