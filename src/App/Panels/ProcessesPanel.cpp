#include "ProcessesPanel.h"

#include "App/KeyboardInput.h"
#include "App/Panel.h"
#include "App/PanelPlatforms.h"
#include "App/Panels/ExitingProcessRows.h"
#include "App/Panels/FindHandlesDialog.h"
#include "App/Panels/GpuSection.h"
#include "App/Panels/IncrementalSearch.h"
#include "App/Panels/NewProcessHighlight.h"
#include "App/Panels/NiceStep.h"
#include "App/Panels/ProcessActionConfirm.h"
#include "App/Panels/ProcessActionsView.h"
#include "App/Panels/ProcessAffinityDialog.h"
#include "App/Panels/ProcessBatchAction.h"
#include "App/Panels/ProcessBatchRun.h"
#include "App/Panels/ProcessColumnAvailability.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessDisplayFreeze.h"
#include "App/Panels/ProcessDumpView.h"
#include "App/Panels/ProcessFilterCache.h"
#include "App/Panels/ProcessGrouping.h"
#include "App/Panels/ProcessLookup.h"
#include "App/Panels/ProcessPins.h"
#include "App/Panels/ProcessRowFormat.h"
#include "App/Panels/ProcessRowMeter.h"
#include "App/Panels/ProcessRowMeterView.h"
#include "App/Panels/ProcessRowSelection.h"
#include "App/Panels/ProcessSelection.h"
#include "App/Panels/ProcessSortUtils.h"
#include "App/Panels/ProcessStateColor.h"
#include "App/Panels/ProcessTableFlags.h"
#include "App/Panels/ProcessTableLayout.h"
#include "App/Panels/ProcessTableNavigation.h"
#include "App/Panels/ProcessTableSettings.h"
#include "App/Panels/ProcessTreeBadge.h"
#include "App/Panels/ProcessTreeFlatten.h"
#include "App/Panels/ProcessTreeIndent.h"
#include "App/Panels/ProcessTypeColor.h"
#include "App/Panels/RunTaskDialog.h"
#include "App/Panels/RunTaskHistory.h"
#include "App/Panels/StuckProcesses.h"
#include "App/PlatformOpen.h"
#include "App/ProcessColumnConfig.h"
#include "App/SelectOverride.h"
#include "App/SyntheticScenario.h"
#include "App/UserConfig.h"
#include "AuthorizeWording.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/DeferredProbe.h"
#include "Domain/GPUModel.h"
#include "Domain/HandleSearch.h"
#include "Domain/Numeric.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/ProcessState.h"
#include "Domain/SamplingConfig.h"
#include "Platform/CpuAffinity.h"
#include "Platform/CurrentProcess.h"
#include "Platform/IProcessActions.h"
#include "Platform/ProcessTypes.h"
#include "Platform/ThreadName.h"
#include "UI/CellText.h"
#include "UI/EmptyState.h"
#include "UI/Format.h"
#include "UI/FrameOutline.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

// clang-format off
#include <imgui.h>
#include <imgui_internal.h> // ImGuiTable: the laid-out column widths, and the table background channel, have no public accessor
#include <misc/cpp/imgui_stdlib.h>
#include <spdlog/spdlog.h>
// clang-format on

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace App
{

namespace
{

// The tree's indent per level and the name width it must leave are font-relative; see
// ProcessTreeIndent::INDENT_PER_LEVEL_EM and MIN_NAME_WIDTH_EM.
//
// The name reservation bounds the tree indent so the expand/collapse button can never be pushed
// out of its cell, which would make a deep parent impossible to toggle (#906). The indent gives
// way first, because depth is also conveyed by the expander column alignment and the PPID column,
// whereas a truncated name has no other source (#906, #913).

// The units the mixed-unit columns can show, exactly as their cells print them. Each column's unit
// slot is as wide as the widest of its units, measured in the current font (#1201).
constexpr std::array<std::string_view, 3> POWER_UNITS = {" W", " mW", " µW"};

// Static UI labels (cached for text size measurements). The view-mode control is two segments, the
// current mode's drawn selected, so it shows the mode it is in rather than the one it switches to (#1209).
constexpr std::string_view LIST_VIEW_LABEL = ICON_FA_LIST " List";
constexpr std::string_view TREE_VIEW_LABEL = ICON_FA_SITEMAP " Tree";
constexpr std::string_view COLUMNS_LABEL = ICON_FA_TABLE_COLUMNS " Columns";
constexpr const char* COLUMNS_POPUP_ID = "##ColumnsMenu";
constexpr const char* SORT_MENU_POPUP_ID = "##SortMenu"; // F6 (#3097)
constexpr const char* ROW_MENU_POPUP_ID = "##ProcessRowMenu";
constexpr const char* FILTER_HINT = ICON_FA_MAGNIFYING_GLASS "  Filter by name or command...";
// Tree view's Expand all and Collapse all (#838): icon buttons, explained by their tooltips.
constexpr const char* EXPAND_ALL_BUTTON = ICON_FA_ANGLES_DOWN "##ExpandAll";
constexpr const char* COLLAPSE_ALL_BUTTON = ICON_FA_ANGLES_UP "##CollapseAll";
// How opaque a filtered tree draws an ancestor that is there only to place a match (#838).
constexpr float CONTEXT_ROW_ALPHA = 0.5F;
// Shown beside the process count while a held Ctrl freezes the pane (#928).
constexpr const char* FROZEN_LABEL = ICON_FA_PAUSE " Paused (Ctrl)";
// The narrow-window form: measureToolbarMinimumWidth() reserves room for this one.
constexpr const char* FROZEN_ICON = ICON_FA_PAUSE;

/// True when any keyboard key other than a modifier is held: Ctrl with one of these is a shortcut
/// (Ctrl+C, Ctrl+=), not the freeze gesture (#928). Only the keyboard block of ImGuiKey is walked;
/// the gamepad and mouse keys follow ImGuiKey_Oem102.
[[nodiscard]] bool anyNonModifierKeyDown()
{
    for (int k = ImGuiKey_Tab; k <= ImGuiKey_Oem102; ++k)
    {
        if (k >= ImGuiKey_LeftCtrl && k <= ImGuiKey_RightSuper)
        {
            continue;
        }
        if (ImGui::IsKeyDown(static_cast<ImGuiKey>(k)))
        {
            return true;
        }
    }
    return false;
}

// How long a row-menu action's result stays in the toolbar, like the Actions block's (#1209).
constexpr float ROW_ACTION_RESULT_SECONDS = 5.0F;

[[nodiscard]] float measureTextWidth(std::string_view text)
{
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

/// One segment of the List | Tree view-mode control (#1209): a button of `width`, drawn in the
/// selection colour while `selected`. Returns true when an unselected segment is pressed.
bool viewModeSegment(std::string_view label, bool selected, float width)
{
    if (selected)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_Header));
    }
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - the labels are constexpr literals
    const bool pressed = ImGui::Button(label.data(), ImVec2(width, 0.0F));
    if (selected)
    {
        ImGui::PopStyleColor(3);
    }
    return pressed && !selected;
}

/// Marks the row just submitted -- a Selectable spanning all columns -- as a multi-selection's primary
/// row (#1926): a bar at its left edge and an outline, in @p color, over its selection fill. Drawn in
/// the table's background channel, where the Selectable drew that fill, so the cells' text stays on
/// top, and held to the part of the row in view (ProcessRowSelection::markerFor()).
void drawPrimaryRowMarker(const ImVec4& color)
{
    ImGui::TablePushBackgroundChannel();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ProcessRowSelection::Marker marker = ProcessRowSelection::markerFor(
        ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), drawList->GetClipRectMin().x, drawList->GetClipRectMax().x, ImGui::GetFontSize());
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(color);
    drawList->AddRect(marker.outlineMin, marker.outlineMax, col, 0.0F, ImDrawFlags_None, marker.outlineThickness);
    drawList->AddRectFilled(marker.barMin, marker.barMax, col);
    ImGui::TablePopBackgroundChannel();
}

[[nodiscard]] constexpr auto toImGuiId(ProcessColumn col) noexcept -> ImGuiID
{
    // Safe: ProcessColumn is a small uint8_t-backed enum; ImGuiID is a wider unsigned type.
    return ImGuiID{std::to_underlying(col)};
}

[[nodiscard]] auto columnFromUserId(ImGuiID id) -> std::optional<ProcessColumn>
{
    // Map back via known columns to avoid integer->enum casts.
    for (const ProcessColumn col : allProcessColumns())
    {
        if (toImGuiId(col) == id)
        {
            return col;
        }
    }

    return std::nullopt;
}

using UI::Widgets::CLIPPED_CELL_TOOLTIP_WRAP_EM;

/// Column header tooltips wrap at this many ems.
constexpr float HEADER_TOOLTIP_WRAP_EM = 32.0F;

/// Draws `text` in the current table cell, left- or right-aligned, using an already-measured
/// `textWidth`: ellipsized with the full value as its tooltip when it does not fit (#914), through
/// the cell helper every table shares (UI::Widgets::cellText(), #1920). The text is measured once --
/// ImFontCalcTextSizeEx showed up as a real cost in interactive-frame profiling -- and a clipped cell
/// takes only its own width in the layout, as it always has here.
///
/// @param clippedTooltip  Whether a clipped cell gets the full text as its tooltip; false for a cell
///                        that draws its own tooltip (the Services column's list, #1880).
void renderCellText(std::string_view text, float textWidth, bool rightAligned, bool clippedTooltip = true)
{
    static_cast<void>(
        UI::Widgets::cellText(text, textWidth, {.rightAligned = rightAligned, .clippedTooltip = clippedTooltip, .layoutFullText = false}));
}

/// The depth guide lines of a tree row at `depth` (#838), drawn from the start of its Name cell, where
/// the cursor is: one subtle vertical line per ancestor level, through the middle of that ancestor's
/// expander, from the row's top to its bottom, so the lines of consecutive rows join. In the theme's
/// tree-line colour (ImGuiCol_TreeLines, held to a contrast floor on the rows) and ImGui's tree-line
/// thickness, scaled with the style.
void drawTreeGuideLines(int depth, float indentPerLevel, float expanderWidth)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 cellStart = ImGui::GetCursorScreenPos();
    const float top = cellStart.y - style.CellPadding.y;
    const float bottom = cellStart.y + ImGui::GetTextLineHeight() + style.CellPadding.y;
    const ImU32 color = ImGui::GetColorU32(UI::Theme::get().scheme().treeLines);
    const float thickness = std::max(1.0F, std::round(style.TreeLinesSize));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (int level = 0; level < depth; ++level)
    {
        // On a pixel centre, so a 1px line is crisp rather than smeared over two columns.
        const float x = std::floor(cellStart.x + ProcessTreeIndent::guideLineX(level, indentPerLevel, expanderWidth)) + 0.5F;
        drawList->AddLine(ImVec2(x, top), ImVec2(x, bottom), color, thickness);
    }
}

/// Draws `text` (already measured as `textWidth`) at the cursor in at most `width`: as is when it
/// fits, otherwise ellipsized with the full text as its tooltip, as a clipped table cell is. For the
/// toolbar's status text, which must not grow past its slot (#1209).
void renderBoundedText(std::string_view text, float textWidth, float width)
{
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
    {
        return;
    }
    const bool clipped = ProcessTableLayout::isCellTextClipped(textWidth, width);
    const float itemWidth = clipped ? std::max(width, 0.0F) : textWidth;
    const char* textBegin = text.data();
    const char* textEnd = text.data() + text.size();
    const ImVec2 textPos(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
    const ImVec2 itemSize(itemWidth, ImGui::GetFontSize());
    const ImRect bounds(textPos, ImVec2(textPos.x + itemSize.x, textPos.y + itemSize.y));
    ImGui::ItemSize(itemSize, 0.0F);
    if (!ImGui::ItemAdd(bounds, 0))
    {
        return;
    }
    if (!clipped)
    {
        ImGui::RenderText(textPos, textBegin, textEnd, false);
        return;
    }
    const ImVec2 measuredSize(textWidth, itemSize.y);
    ImGui::RenderTextEllipsis(window->DrawList, bounds.Min, bounds.Max, bounds.Max.x, textBegin, textEnd, &measuredSize);
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * CLIPPED_CELL_TOOLTIP_WRAP_EM);
        ImGui::TextUnformatted(textBegin, textEnd);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/// Renders `text` right-aligned within the remaining cell width, using an already-measured
/// `textWidth` instead of calling ImGui::CalcTextSize() itself -- callers own measuring (and,
/// for RowFormatCache-backed columns, caching) that width. See the AlignedCellText
/// overload below for the common case.
void renderRightAlignedText(std::string_view text, float textWidth)
{
    renderCellText(text, textWidth, /*rightAligned=*/true);
}

/// Draws the cells of a measured zero or a missing value muted (#1210) for as long as it lives;
/// pushes nothing for a measured value.
class MutedCellScope
{
  public:
    explicit MutedCellScope(bool muted) : m_Muted(muted)
    {
        if (m_Muted)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::get().scheme().textMuted);
        }
    }
    ~MutedCellScope()
    {
        if (m_Muted)
        {
            ImGui::PopStyleColor();
        }
    }
    MutedCellScope(const MutedCellScope&) = delete;
    MutedCellScope& operator=(const MutedCellScope&) = delete;
    MutedCellScope(MutedCellScope&&) = delete;
    MutedCellScope& operator=(MutedCellScope&&) = delete;

  private:
    bool m_Muted;
};

/// The tooltip of a cell with no value, saying why (#1210); nothing for a cell that has one.
void setUnavailableReasonTooltip(const char* reason)
{
    if (reason != nullptr)
    {
        ImGui::SetItemTooltip("%s", reason);
    }
}

/// Common case: a RowFormatCache-backed cell whose text is built lazily, on demand, only for
/// rows that are actually rendered (see renderProcessRow()'s get-or-build lookup), and whose
/// width is likewise measured lazily -- the first time this specific cell is actually drawn --
/// and cached into `cell.width` (mutable; see AlignedCellText's doc comment for why this must be
/// lazy rather than done for every process at cache-population time). ImFontCalcTextSizeEx and
/// ImGui::ItemSize showed up as real, non-trivial costs in interactive-frame profiling; this
/// keeps that cost paid at most once per visible row per cache generation, instead of every
/// frame or (worse) for every process regardless of visibility.
void renderRightAlignedText(const AlignedCellText& cell)
{
    if (cell.width < 0.0F)
    {
        cell.width = ImGui::CalcTextSize(cell.text.c_str(), cell.text.c_str() + cell.text.size()).x;
    }
    {
        const MutedCellScope muted(cell.tone != ProcessRowFormat::CellTone::Value);
        renderRightAlignedText(cell.text, cell.width);
    }
    setUnavailableReasonTooltip(cell.unavailableReason);
}

/// Renders a cell of a mixed-unit column ("512.0 B", "1.5 KiB", "3.2 MiB") decimal-aligned (#1201):
/// the unit in a slot `unitSlotWidth` wide at the cell's right edge, the number right-aligned
/// against it, so the decimal points of every row line up (see
/// ProcessTableLayout::layoutUnitAlignedCell()). A cell with no unit (the unavailable dash) is
/// right-aligned against the slot too, under the numbers; one too narrow for its number and the slot
/// is drawn clipped, as renderRightAlignedText() does. Widths are measured once per cache entry,
/// like renderRightAlignedText()'s.
void renderUnitAlignedText(const AlignedCellText& cell, float unitSlotWidth)
{
    const std::string_view number = cell.number();
    const std::string_view unit = cell.unit();
    if (cell.width < 0.0F)
    {
        cell.width = ImGui::CalcTextSize(cell.text.c_str(), cell.text.c_str() + cell.text.size()).x;
    }
    if (cell.numberWidth < 0.0F)
    {
        cell.numberWidth = cell.hasUnit() ? ImGui::CalcTextSize(number.data(), number.data() + number.size()).x : cell.width;
    }
    const auto layout = ProcessTableLayout::layoutUnitAlignedCell(
        cell.numberWidth, cell.width - cell.numberWidth, unitSlotWidth, ImGui::GetContentRegionAvail().x);
    if (!layout.fits)
    {
        renderRightAlignedText(cell);
        return;
    }

    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
    {
        return;
    }
    // The same item placement as renderCellText(): one item from the number's left edge to the
    // unit's right edge, the cursor advanced past the alignment offset as well.
    const ImVec2 origin(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
    const ImVec2 numberPos(origin.x + layout.numberX, origin.y);
    const ImVec2 itemSize(layout.itemWidth, ImGui::GetFontSize());
    const ImRect bounds(numberPos, ImVec2(numberPos.x + itemSize.x, numberPos.y + itemSize.y));
    ImGui::ItemSize(ImVec2(layout.numberX + itemSize.x, itemSize.y), 0.0F);
    if (!ImGui::ItemAdd(bounds, 0))
    {
        return;
    }
    {
        const MutedCellScope muted(cell.tone != ProcessRowFormat::CellTone::Value);
        ImGui::RenderText(numberPos, number.data(), number.data() + number.size(), false);
        if (!unit.empty())
        {
            ImGui::RenderText(ImVec2(origin.x + layout.unitX, origin.y), unit.data(), unit.data() + unit.size(), false);
        }
    }
    setUnavailableReasonTooltip(cell.unavailableReason);
}

/// A free-text cell with no value (#1210): the unavailable dash, muted, left-aligned, with `reason`
/// as its tooltip. `dashWidth` is ProcessRowFormat::UNAVAILABLE_CELL_TEXT's width in the current font.
void renderUnavailableTextCell(const char* reason, float dashWidth)
{
    {
        const MutedCellScope muted(true);
        renderCellText(ProcessRowFormat::UNAVAILABLE_CELL_TEXT, dashWidth, /*rightAligned=*/false);
    }
    setUnavailableReasonTooltip(reason);
}

void renderLeftAlignedText(std::string_view text, const ProcessRowFormat::LazyTextWidth& width);

/// A free-text cell as ProcessColumnAvailability::textCell() decided: the text, nothing, or the
/// "not available on this system" dash.
void renderTextCell(ProcessColumnAvailability::TextCell content,
                    std::string_view text,
                    const ProcessRowFormat::LazyTextWidth& width,
                    float dashWidth)
{
    switch (content)
    {
    case ProcessColumnAvailability::TextCell::Text:
        renderLeftAlignedText(text, width);
        break;
    case ProcessColumnAvailability::TextCell::Unavailable:
        renderUnavailableTextCell(ProcessRowFormat::UNSUPPORTED_CELL_REASON, dashWidth);
        break;
    case ProcessColumnAvailability::TextCell::Blank:
        break;
    }
}

/// Width of the widest of `units` in the current font.
template<typename Units> [[nodiscard]] float widestTextWidth(const Units& units)
{
    float widest = 0.0F;
    for (const std::string_view unit : units)
    {
        widest = std::max(widest, ImGui::CalcTextSize(unit.data(), unit.data() + unit.size()).x);
    }
    return widest;
}

/// The width of `text` as drawn in the current font, from `width` once it has been measured: measured
/// here the first time the cell is drawn, then reused until its RowFormatCache entry is rebuilt for a
/// new snapshot or font (#1141).
[[nodiscard]] float cachedTextWidth(std::string_view text, const ProcessRowFormat::LazyTextWidth& width)
{
    return width.get([text] { return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x; });
}

/// Renders free text (a name, a user, a command line) left-aligned in the current cell. `width` is
/// the cell's slot in its row's RowFormatCache entry, so the text is measured once per entry rather
/// than every frame (#1141): command lines run to thousands of characters, each a glyph lookup.
void renderLeftAlignedText(std::string_view text, const ProcessRowFormat::LazyTextWidth& width)
{
    renderCellText(text, cachedTextWidth(text, width), /*rightAligned=*/false);
}

// ============================================================================
// Per-column cell renderers (#1382)
// ============================================================================
//
// Every column but PID and Name is drawn by one small function, looked up by
// column in CELL_RENDERERS below: its format (which RowFormatCache or snapshot
// field), alignment (left, right, centred, or decimal-aligned against a unit
// slot) and N/A handling live in that one entry. PID (the row's selectable) and
// Name (the tree indent and expander) need the panel's selection and tree
// state, so renderProcessRow() draws them itself. A plain function pointer per
// column: no allocation and no std::function per cell, and the lookup is an
// array index.

/// Draws one cell of the current row in the current table column.
using ProcessCellRenderer = void (*)(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& widths);

/// A RowFormatCache cell right-aligned: a number, a duration, a date, an ID, a
/// percentage, or "-".
template<AlignedCellText RowFormatCache::* Cell>
void rightAlignedCell(const Domain::ProcessSnapshot& /*proc*/, const RowFormatCache& fmt, const ProcessCellWidths& /*widths*/)
{
    renderRightAlignedText(fmt.*Cell);
}

/// A RowFormatCache cell of a mixed-unit column, decimal-aligned against the
/// column's unit slot (#1201).
template<AlignedCellText RowFormatCache::* Cell, float ProcessCellWidths::* UnitSlot>
void unitAlignedCell(const Domain::ProcessSnapshot& /*proc*/, const RowFormatCache& fmt, const ProcessCellWidths& widths)
{
    renderUnitAlignedText(fmt.*Cell, widths.*UnitSlot);
}

/// Free text from the snapshot, left-aligned.
template<std::string Domain::ProcessSnapshot::* Text, ProcessRowFormat::LazyTextWidth RowFormatCache::* Width>
void leftAlignedCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& /*widths*/)
{
    renderLeftAlignedText(proc.*Text, fmt.*Width);
}

/// Free text from the snapshot, left-aligned; blank when the process has none, and the unavailable
/// dash when this system cannot fill the column at all (`Supported`, ProcessColumnAvailability::
/// textCell(), #1210). Blank, not a dash, for an empty value: no status is a process in neither
/// state the column names, no GPU device is one using none, no container is a process in none, and an
/// empty publisher is either an executable without one or one that could not be read -- the probe
/// cannot tell which.
template<std::string Domain::ProcessSnapshot::* Text,
         ProcessRowFormat::LazyTextWidth RowFormatCache::* Width,
         bool RowFormatCache::* Supported>
void leftAlignedOrBlankCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& widths)
{
    renderTextCell(
        ProcessColumnAvailability::textCell(fmt.*Supported, !(proc.*Text).empty()), proc.*Text, fmt.*Width, widths.unavailableText);
}

/// The services the process hosts (#1880): their names comma-joined, left-aligned; blank for none, and
/// the unavailable dash where this system cannot tell. Hovering the names lists them one per line,
/// whether or not the cell had room for them all.
void servicesCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& widths)
{
    const ProcessColumnAvailability::TextCell content =
        ProcessColumnAvailability::textCell(fmt.servicesSupported, !proc.serviceNames.empty());
    if (content != ProcessColumnAvailability::TextCell::Text)
    {
        renderTextCell(content, proc.services, fmt.servicesWidth, widths.unavailableText);
        return;
    }
    renderCellText(proc.services, cachedTextWidth(proc.services, fmt.servicesWidth), /*rightAligned=*/false, /*clippedTooltip=*/false);
    if (ImGui::BeginItemTooltip())
    {
        const std::string heading = ProcessRowFormat::servicesTooltipHeading(proc.serviceNames.size());
        ImGui::TextColored(UI::Theme::get().scheme().textMuted, "%s", heading.c_str());
        std::size_t index = 0;
        while (index < proc.serviceNames.size())
        {
            ImGui::TextUnformatted(proc.serviceNames[index].c_str());
            ++index;
        }
        ImGui::EndTooltip();
    }
}

/// A GPU or NPU text cell (engines, devices): blank for none, the "not available on this system" dash
/// where per-process usage cannot be observed (@p supported false), and the unreadable dash where it
/// can but this generation's read failed or the process started since the last read (#1210).
void renderGpuTextCell(
    std::string_view text, const ProcessRowFormat::LazyTextWidth& width, const RowFormatCache& fmt, bool supported, float dashWidth)
{
    if (supported && (fmt.gpuUnreadReason != nullptr))
    {
        renderUnavailableTextCell(fmt.gpuUnreadReason, dashWidth);
        return;
    }
    renderTextCell(ProcessColumnAvailability::textCell(supported, !text.empty()), text, width, dashWidth);
}

/// The GPU engines in use: RowFormatCache's comma-joined list, left-aligned (renderGpuTextCell()).
void gpuEngineCell(const Domain::ProcessSnapshot& /*proc*/, const RowFormatCache& fmt, const ProcessCellWidths& widths)
{
    renderGpuTextCell(fmt.gpuEngines, fmt.gpuEnginesWidth, fmt, fmt.gpuSupported, widths.unavailableText);
}

/// The GPUs the process uses, left-aligned (renderGpuTextCell()).
void gpuDeviceCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& widths)
{
    renderGpuTextCell(proc.gpuDevices, fmt.gpuDevicesWidth, fmt, fmt.gpuSupported, widths.unavailableText);
}

/// The NPU engines in use (#1799), as gpuEngineCell() draws the GPU's.
void npuEngineCell(const Domain::ProcessSnapshot& /*proc*/, const RowFormatCache& fmt, const ProcessCellWidths& widths)
{
    renderGpuTextCell(fmt.npuEngines, fmt.npuEnginesWidth, fmt, fmt.npuSupported, widths.unavailableText);
}

/// The kernel-style state code, centred, in its state colour.
void stateCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& /*fmt*/, const ProcessCellWidths& /*widths*/)
{
    // The kernel-style code, not the name's first letter: Stopped is T, Dead is X
    // (#1352).
    const char stateChar = Domain::processStateCode(proc.displayState);
    const ImVec4 stateColor = processStateColor(stateChar, UI::Theme::get().scheme());

    // Center the state character in the column
    const std::array<char, 2> stateStr = {stateChar, '\0'};
    const float textWidth = ImGui::CalcTextSize(stateStr.data()).x;
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float offset = std::max(0.0F, (availWidth - textWidth) * 0.5F);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

    ImGui::PushStyleColor(ImGuiCol_Text, stateColor);
    ImGui::TextUnformatted(stateStr.data());
    ImGui::PopStyleColor();
}

/// The priority label, right-aligned: the platform's class name where it has one (Windows:
/// Realtime is not High, #1280), else the nice value's label.
void priorityCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& /*fmt*/, const ProcessCellWidths& widths)
{
    // getProcessPriorityLabel returns string_view into static storage — no allocation
    // needed. Not RowFormatCache-backed (it's a direct lookup, not a per-row formatted
    // string), so its width comes from ProcessCellWidths' small fixed-label width cache
    // instead of an AlignedCellText.
    const std::string_view priorityLabel = Domain::Priority::getProcessPriorityLabel(proc.priorityClass, proc.nice);
    renderRightAlignedText(priorityLabel, widths.priorityLabelWidth(priorityLabel));
}

/// The command line, left-aligned, or the name in brackets when there is none
/// (a kernel thread, or a process whose command line could not be read).
void commandCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& /*widths*/)
{
    if (!proc.command.empty())
    {
        renderLeftAlignedText(proc.command, fmt.commandWidth);
    }
    else
    {
        // Show name in brackets if no command line available
        ImGui::Text("[%s]", proc.name.c_str());
    }
}

/// The process type, left-aligned in its type colour (shared with Process
/// Details, #1180); the unavailable dash where this system cannot classify
/// processes, or could not classify this one (#1210).
void typeCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& widths)
{
    if (!fmt.processTypeSupported)
    {
        renderUnavailableTextCell(ProcessRowFormat::UNSUPPORTED_CELL_REASON, widths.unavailableText);
    }
    else if (!proc.processType.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, processTypeColor(proc.processType, UI::Theme::get().scheme()));
        renderLeftAlignedText(proc.processType, fmt.processTypeWidth);
        ImGui::PopStyleColor();
    }
    else
    {
        renderUnavailableTextCell(ProcessRowFormat::UNREADABLE_CELL_REASON, widths.unavailableText);
    }
}

/// The architecture the process runs as (#2394), left-aligned; the unavailable dash where this system
/// cannot read architectures, or could not read this process's (another user's process, a kernel thread).
void architectureCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, const ProcessCellWidths& widths)
{
    if (!fmt.architectureSupported)
    {
        renderUnavailableTextCell(ProcessRowFormat::UNSUPPORTED_CELL_REASON, widths.unavailableText);
    }
    else if (!proc.architecture.empty())
    {
        renderLeftAlignedText(proc.architecture, fmt.architectureWidth);
    }
    else
    {
        renderUnavailableTextCell(ProcessRowFormat::UNREADABLE_CELL_REASON, widths.unavailableText);
    }
}

/// One CELL_RENDERERS entry: the column it draws (checked against its index
/// below) and its renderer, or nullptr for a column renderProcessRow() draws.
struct ProcessCellEntry
{
    ProcessColumn column;
    ProcessCellRenderer render;
};

/// The renderer for every column but PID and Name, indexed by toIndex(column).
/// Initialised directly, not by a consteval builder, so static analysers
/// (CodeQL cpp/unused-static-function, #1403) see every renderer referenced.
constexpr std::array<ProcessCellEntry, processColumnCount()> CELL_RENDERERS = {{
    {.column = ProcessColumn::PID, .render = nullptr},  // drawn by renderProcessRow() (the selectable)
    {.column = ProcessColumn::Name, .render = nullptr}, // drawn by renderProcessRow() (tree indent and expander)
    // Identity
    {.column = ProcessColumn::User, .render = &leftAlignedCell<&Domain::ProcessSnapshot::user, &RowFormatCache::userWidth>},
    {.column = ProcessColumn::PPID, .render = &rightAlignedCell<&RowFormatCache::ppid>},
    {.column = ProcessColumn::Publisher,
     .render = &leftAlignedOrBlankCell<&Domain::ProcessSnapshot::publisher,
                                       &RowFormatCache::publisherWidth,
                                       &RowFormatCache::publisherSupported>},
    {.column = ProcessColumn::Container,
     .render = &leftAlignedOrBlankCell<&Domain::ProcessSnapshot::container,
                                       &RowFormatCache::containerWidth,
                                       &RowFormatCache::containerSupported>},
    {.column = ProcessColumn::Services, .render = &servicesCell},
    // State
    {.column = ProcessColumn::State, .render = &stateCell},
    {.column = ProcessColumn::Status,
     .render = &leftAlignedOrBlankCell<&Domain::ProcessSnapshot::status, &RowFormatCache::statusWidth, &RowFormatCache::statusSupported>},
    {.column = ProcessColumn::Type, .render = &typeCell},
    {.column = ProcessColumn::Architecture, .render = &architectureCell},
    // Resources
    {.column = ProcessColumn::CpuPercent, .render = &rightAlignedCell<&RowFormatCache::cpuPercent>},
    {.column = ProcessColumn::MemPercent, .render = &rightAlignedCell<&RowFormatCache::memPercent>},
    {.column = ProcessColumn::Resident, .render = &unitAlignedCell<&RowFormatCache::resident, &ProcessCellWidths::unitBytes>},
    {.column = ProcessColumn::Virtual, .render = &unitAlignedCell<&RowFormatCache::virtualMem, &ProcessCellWidths::unitBytes>},
    {.column = ProcessColumn::Shared, .render = &unitAlignedCell<&RowFormatCache::shared, &ProcessCellWidths::unitBytes>},
    {.column = ProcessColumn::PeakResident, .render = &unitAlignedCell<&RowFormatCache::peakRss, &ProcessCellWidths::unitBytes>},
    {.column = ProcessColumn::Private, .render = &unitAlignedCell<&RowFormatCache::privateMem, &ProcessCellWidths::unitBytes>},
    {.column = ProcessColumn::Swap, .render = &unitAlignedCell<&RowFormatCache::swap, &ProcessCellWidths::unitBytes>},
    // Scheduling
    {.column = ProcessColumn::Priority, .render = &priorityCell},
    {.column = ProcessColumn::Affinity, .render = &rightAlignedCell<&RowFormatCache::affinity>},
    {.column = ProcessColumn::Threads, .render = &rightAlignedCell<&RowFormatCache::threads>},
    {.column = ProcessColumn::Handles, .render = &rightAlignedCell<&RowFormatCache::handles>},
    // Unavailable only when the probe could not read the count (process not accessible).
    // A count of 0 is a valid result for non-GUI background processes and is
    // shown as a muted "0" (#1210).
    {.column = ProcessColumn::GdiObjects, .render = &rightAlignedCell<&RowFormatCache::gdiObjects>},
    // Time
    {.column = ProcessColumn::CpuTime, .render = &rightAlignedCell<&RowFormatCache::cpuTime>},
    {.column = ProcessColumn::StartTime, .render = &rightAlignedCell<&RowFormatCache::startTime>},
    // I/O
    {.column = ProcessColumn::IoRead, .render = &unitAlignedCell<&RowFormatCache::ioRead, &ProcessCellWidths::unitBytesPerSec>},
    {.column = ProcessColumn::IoWrite, .render = &unitAlignedCell<&RowFormatCache::ioWrite, &ProcessCellWidths::unitBytesPerSec>},
    {.column = ProcessColumn::PageFaults, .render = &rightAlignedCell<&RowFormatCache::pageFaults>},
    // Network
    {.column = ProcessColumn::NetSent, .render = &unitAlignedCell<&RowFormatCache::netSent, &ProcessCellWidths::unitBytesPerSec>},
    {.column = ProcessColumn::NetReceived, .render = &unitAlignedCell<&RowFormatCache::netRecv, &ProcessCellWidths::unitBytesPerSec>},
    // Power
    {.column = ProcessColumn::Power, .render = &unitAlignedCell<&RowFormatCache::power, &ProcessCellWidths::unitPower>},
    // GPU
    {.column = ProcessColumn::GpuPercent, .render = &rightAlignedCell<&RowFormatCache::gpuPercent>},
    {.column = ProcessColumn::GpuMemory, .render = &unitAlignedCell<&RowFormatCache::gpuMemory, &ProcessCellWidths::unitBytes>},
    {.column = ProcessColumn::GpuEngine, .render = &gpuEngineCell},
    {.column = ProcessColumn::GpuDevice, .render = &gpuDeviceCell},
    // NPU (#1799)
    {.column = ProcessColumn::NpuPercent, .render = &rightAlignedCell<&RowFormatCache::npuPercent>},
    {.column = ProcessColumn::NpuMemory, .render = &unitAlignedCell<&RowFormatCache::npuMemory, &ProcessCellWidths::unitBytes>},
    {.column = ProcessColumn::NpuEngine, .render = &npuEngineCell},
    // Command
    {.column = ProcessColumn::Command, .render = &commandCell},
}};

// Every entry sits at its column's index, every column but PID and Name has a
// renderer, and those two have none: a column added to ProcessColumn without
// an entry, or an entry out of order, fails to compile here instead of drawing
// the wrong cell or an empty one.
static_assert(std::ranges::all_of(allProcessColumns(),
                                  [](ProcessColumn col)
                                  {
                                      const ProcessCellEntry& entry = CELL_RENDERERS[toIndex(col)];
                                      const bool drawnByRow = (col == ProcessColumn::PID) || (col == ProcessColumn::Name);
                                      return (entry.column == col) && ((entry.render != nullptr) != drawnByRow);
                                  }),
              "CELL_RENDERERS needs one entry per Processes column, in ProcessColumn "
              "order, with a renderer for every column but PID and Name");

} // namespace

// ============================================================================
// Column layout persistence (#952)
// ============================================================================

void ProcessesPanel::restoreTableLayout(std::string_view stored)
{
    if (ImGui::GetCurrentContext() == nullptr)
    {
        return;
    }

    // Filtered before ImGui sees it: the text comes from a user-editable file, and only a single
    // table section may be passed on -- never a window position or a docking layout. The default
    // column order is made explicit, or a layout saved with only a sort line -- by this version or an
    // older one -- would come back with the sorted column first (#1393).
    const std::string layout = ProcessTableSettings::withExplicitOrder(stored);
    if (!layout.empty())
    {
        ImGui::LoadIniSettingsFromMemory(layout.data(), layout.size());
    }
}

std::string ProcessesPanel::captureTableLayout() const
{
    if (m_TableId == 0 || ImGui::GetCurrentContext() == nullptr)
    {
        return {};
    }

    std::size_t iniSize = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&iniSize);
    if (ini == nullptr)
    {
        return {};
    }
    std::string layout = ProcessTableSettings::extractTableSection(std::string_view(ini, iniSize), m_TableId);

    // A section with no sort at all lost it to tree view (see m_SortBackupLayout); put back the
    // sort list view last had. A section that has a sort is newer and is left alone.
    if (!m_SortBackupLayout.empty())
    {
        layout = ProcessTableSettings::carrySortForward(layout, m_SortBackupLayout);
    }
    // Tree view's automatic Name width is not the list's: the next launch opens in list view, so save
    // the width Name had before tree view widened it, unless the user has resized it since (#1209).
    if (const ImGuiTable* table = ImGui::TableFindByID(m_TableId); table != nullptr)
    {
        const auto nameIdx = static_cast<int>(toIndex(ProcessColumn::Name));
        if (nameIdx < table->ColumnsCount)
        {
            if (const std::optional<float> listWidth = ProcessTreeIndent::nameWidthToSave(
                    m_TreeViewEnabled, m_NameWidthSetForTree, table->Columns[nameIdx].WidthRequest, m_NameWidthBeforeTree))
            {
                layout =
                    ProcessTableSettings::withColumnWidth(layout, toIndex(ProcessColumn::Name), static_cast<int>(std::lround(*listWidth)));
            }
        }
    }
    // Last, so a sort line carried into a section with no order lines gets them too (#1393).
    return ProcessTableSettings::withExplicitOrder(layout);
}

// ============================================================================
// TextSizeCache implementation
// ============================================================================

bool ProcessesPanel::TextSizeCache::isValid() const noexcept
{
    // Cache invalid if not yet populated, if the font has changed, or if the font atlas was rebuilt
    // (which can give the new font the old one's address, #943)
    return fontPtr != nullptr && fontPtr == ImGui::GetFont() && fontGeneration == UI::Theme::get().fontGeneration();
}

void ProcessesPanel::TextSizeCache::populate()
{
    // Store current font pointer and atlas generation for invalidation detection, and a new stamp
    // for the row-format cache
    fontPtr = ImGui::GetFont();
    fontGeneration = UI::Theme::get().fontGeneration();
    ++stamp;

    // Cache column header widths
    for (const ProcessColumn col : allProcessColumns())
    {
        const auto info = getColumnInfo(col);
        columnHeaderWidths[toIndex(col)] = ImGui::CalcTextSize(info.name.data(), info.name.data() + info.name.size()).x;
    }

    // The unit slot of each mixed-unit column: its widest unit, as the cells print them (#1201)
    std::array<std::string_view, UI::Format::BYTE_UNITS.size()> byteUnits{};
    std::array<std::string_view, UI::Format::BYTE_UNITS.size()> byteRateUnits{};
    for (std::size_t i = 0; i < UI::Format::BYTE_UNITS.size(); ++i)
    {
        byteUnits[i] = UI::Format::cellUnitSuffix(*UI::Format::BYTE_UNITS[i], false);
        byteRateUnits[i] = UI::Format::cellUnitSuffix(*UI::Format::BYTE_UNITS[i], true);
    }
    cells.unitBytes = widestTextWidth(byteUnits);
    cells.unitBytesPerSec = widestTextWidth(byteRateUnits);
    cells.unitPower = widestTextWidth(POWER_UNITS);

    // Cache static label widths
    treeViewLabelWidth = measureTextWidth(TREE_VIEW_LABEL);
    listViewLabelWidth = measureTextWidth(LIST_VIEW_LABEL);
    columnsLabelWidth = measureTextWidth(COLUMNS_LABEL);
    caretRightWidth = measureTextWidth(ICON_FA_CARET_RIGHT);
    caretDownWidth = measureTextWidth(ICON_FA_CARET_DOWN);
    foldIconWidth = std::max(measureTextWidth(ICON_FA_ANGLES_DOWN), measureTextWidth(ICON_FA_ANGLES_UP));
    cells.unavailableText = measureTextWidth(ProcessRowFormat::UNAVAILABLE_CELL_TEXT);

    // Cache Domain::Priority::getProcessPriorityLabel()'s fixed label widths
    for (std::size_t i = 0; i < PRIORITY_LABELS.size(); ++i)
    {
        const auto& label = PRIORITY_LABELS[i];
        cells.priorityLabels[i] = ImGui::CalcTextSize(label.data(), label.data() + label.size()).x;
    }
    cells.widestPriorityLabel = std::ranges::max(cells.priorityLabels);
}

float ProcessesPanel::measureToolbarMinimumWidth()
{
    // Mirrors render()'s toolbar row; see ProcessTableLayout::computeToolbarMinimumWidth().
    const ImGuiStyle& style = ImGui::GetStyle();
    const float emPx = ImGui::GetFontSize();
    const float hintWidth = ImGui::CalcTextSize(FILTER_HINT).x;
    const float filterForHint = hintWidth + (style.FramePadding.x * 2.0F);
    const float filterWanted = ProcessTableLayout::computeFilterWidth(hintWidth, style.FramePadding.x, emPx, 0.0F);

    // The clear button only shows while filtering, but the row must not overlap when it does. The
    // count is the wider of its two forms at a large, fixed count, so the minimum stays put.
    const float clearButton = ImGui::CalcTextSize(ICON_FA_XMARK).x + (style.FramePadding.x * 2.0F);
    const float count =
        std::max(ImGui::CalcTextSize("99,999 processes, 9,999 running").x, ImGui::CalcTextSize("99,999 / 99,999 processes").x);
    // The Columns button and the two view-mode segments, which sit flush against each other (#1209).
    const float columnsButton = measureTextWidth(COLUMNS_LABEL) + (style.FramePadding.x * 2.0F);
    const float viewModeControl = measureTextWidth(LIST_VIEW_LABEL) + measureTextWidth(TREE_VIEW_LABEL) + (style.FramePadding.x * 4.0F);
    // Tree view's Expand all and Collapse all after it (#838), reserved in both views so the minimum
    // stays put when the view changes.
    const float foldButtons =
        (std::max(measureTextWidth(ICON_FA_ANGLES_DOWN), measureTextWidth(ICON_FA_ANGLES_UP)) + (style.FramePadding.x * 2.0F)) * 2.0F;
    // While Ctrl freezes the pane the paused indicator joins the row (#928). Only its icon-only form
    // is reserved: the full label shows when the real count leaves room, which the worst-case count
    // above nearly always does (ProcessTableLayout::choosePausedLabelForm()).
    const float pausedIcon = ImGui::CalcTextSize(FROZEN_ICON).x;
    const float rest = (style.ItemSpacing.x * 7.0F) + clearButton + pausedIcon + count + columnsButton + viewModeControl + foldButtons;
    return ProcessTableLayout::computeToolbarMinimumWidth(filterWanted, filterForHint, rest);
}

void ProcessesPanel::ensureTextSizeCacheValid()
{
    if (!m_TextSizeCache.isValid())
    {
        m_TextSizeCache.populate();
    }
}

// ============================================================================
// ProcessesPanel implementation
// ============================================================================

ProcessesPanel::ProcessesPanel() : Panel("Processes")
{}

ProcessesPanel::ProcessesPanel(std::function<ProcessesPanelPlatform()> makePlatform)
    : Panel("Processes"), m_MakePlatform(std::move(makePlatform))
{}

ProcessesPanel::ProcessesPanel(std::function<ProcessesPanelPlatform()> makePlatform, std::function<void(Core::Event&)> raiseEvent)
    : Panel("Processes"), m_MakePlatform(std::move(makePlatform)), m_RaiseEvent(std::move(raiseEvent))
{}

void ProcessesPanel::raiseEvent(Core::Event& event)
{
    if (m_RaiseEvent)
    {
        m_RaiseEvent(event);
        return;
    }
    Core::Application::get().raiseEvent(event);
}

ProcessesPanel::~ProcessesPanel()
{
    // Order doesn't matter for safety here: BackgroundSampler observes m_ProcessModel via a
    // weak_ptr, so it's never left holding a dangling pointer regardless of which is destroyed
    // first. Stopping the sampler first is still done so no sample() call races the rest of this
    // destructor's cleanup.
    if (m_Sampler)
    {
        m_Sampler->stop();
        m_Sampler.reset();
    }
    m_ProcessModel.reset();
}

void ProcessesPanel::onAttach()
{
    // Load column settings from user config
    m_ColumnSettings = UserConfig::get().settings().processColumns;
    m_ColumnSettings.keepUnhideableColumnsVisible(); // Whatever the settings were given (#1209)
    m_MeterSettings = UserConfig::get().settings().processMeters;
    m_Pins = ProcessPins::Pins(UserConfig::get().settings().pinnedProcesses); // #3061
    m_GroupBy = UserConfig::get().settings().processGroupBy;                  // #3068

    // m_RefreshInterval starts at the SamplingConfig default, and the model at its built-in history
    // length; ShellLayer raises the configured values as events on its first update (#1079).
    m_ForceRefresh = false;

    // The model exists from here, so the shell can wire it (ShellPanels::connectModels()), but without
    // its probe (#2731): the sampler thread builds the platform in its start hook below and seeds the
    // model there, so this thread never enumerates processes or waits for the first read. Until the seed
    // is adopted (onUpdate()) the table has no rows and the capabilities are the defaults.
    // The synthetic scenario's platform when TASKSMACK_SYNTHETIC selects one (#1413), else the
    // platform's. An injected factory (tests, #1546) replaces both, with no scenario.
    const Synthetic::Scenario* scenario = m_MakePlatform ? nullptr : Synthetic::activeScenario();
    m_ProcessModel = std::make_shared<Domain::ProcessModel>(Domain::DEFERRED_PROBE);
    m_OwnPid = Platform::currentProcessId(); // A batch naming TaskSmack itself says so (#804)
    // Config-file only (not in Settings), so applied once here, before the first refresh (#1123).
    m_ProcessModel->setMaxSaneNetworkRate(UserConfig::get().settings().maxSaneRateBps);
    // The sparklines' histories (#1612), kept only for the columns that show one.
    m_ProcessModel->setRowHistoryMetrics(m_MeterSettings.sparklineMetrics());
    // The synthetic scenario starts with its whole history window filled (#1413) by the hook below.
    if (scenario != nullptr)
    {
        m_ProcessModel->setMaxHistorySeconds(Synthetic::startupHistorySeconds(scenario, Domain::Sampling::HISTORY_SECONDS_DEFAULT));
    }

    // What the hook needs from the config, read here: UserConfig belongs to this thread.
    const std::chrono::milliseconds socketStatsCacheTtl(UserConfig::get().settings().socketStatsCacheTtlMs);
    const bool udpEventTrace = UserConfig::get().settings().udpEventTrace;
    m_Handoff = std::make_shared<PlatformHandoff>();

    // Wire sampler: polls the ProcessModel on each interval tick. Seeded by the start hook, so the
    // first background sample waits a full interval after it (#1102).
    Domain::SamplerConfig samplerCfg;
    samplerCfg.interval = m_RefreshInterval;
    samplerCfg.firstSampleAfterInterval = true;
    samplerCfg.threadName = std::string(Platform::PROCESS_SAMPLER_THREAD_NAME);
    samplerCfg.multiThreadedApartment = true; // what the probe creates lives with the sampler, not the UI's STA (#2731)
    // The start hook: on the sampler thread, before its first pass. Everything it touches is captured
    // by value or shared: the model outlives the sampler (onDetach() stops it first), the handoff is
    // shared with this panel, and the scenario is the process's. The factory is still the composition
    // root's (PanelPlatforms) or the test's.
    samplerCfg.onThreadStart =
        [makePlatform = m_MakePlatform, scenario, model = m_ProcessModel, handoff = m_Handoff, socketStatsCacheTtl, udpEventTrace]
    {
        ProcessesPanelPlatform platform = makePlatform ? makePlatform() : makeProcessesPanelPlatform(scenario);
        if (platform.probe)
        {
            platform.probe->setSocketStatsCacheTtl(socketStatsCacheTtl);
            // The opt-in UDP event trace (#1492): the probe reads it; the panel owns its on/off from here on.
            if (platform.udpCollector)
            {
                platform.probe->setUdpTrafficCollector(platform.udpCollector);
                if (udpEventTrace)
                {
                    // Not elevated, or refused: the collector logs why, and the counters stay TCP only.
                    (void) platform.udpCollector->start();
                }
            }
        }
        model->setProbe(std::move(platform.probe));

        // The synthetic scenario's whole history window first (#1413), then the seed continues it.
        if (scenario != nullptr)
        {
            Synthetic::preloadProcessHistory(scenario, *model, std::chrono::steady_clock::now());
        }

        // Seed with one read so the first background sample produces valid CPU deltas instead of
        // all-zero percentages (this call establishes the prev-sample baseline; the next, one interval
        // on, computes the delta).
        model->refresh();

        // The rest of the platform is the UI thread's: the row menu's actions, the icons (GL textures)
        // and the UDP toggle. Handed over here; the panel adopts them on its next frame.
        // Asked here, on this thread, before the actions move: the initializers run in order.
        const Platform::ProcessActionCapabilities actionCapabilities =
            platform.actions ? platform.actions->actionCapabilities() : Platform::ProcessActionCapabilities{};
        const FindHandlesDialog::Capabilities findCapabilities =
            FindHandlesDialog::capabilitiesOf(platform.openFiles.get(), platform.modules.get());
        // The Set affinity dialog's CPU layout, read here once (#3182): GetLogicalProcessorInformationEx or
        // sysfs, which the UI thread never calls. It doesn't change while TaskSmack runs.
        Platform::CpuAffinityLayout affinityLayout =
            (platform.actions && actionCapabilities.canSetAffinity) ? platform.actions->affinityLayout() : Platform::CpuAffinityLayout{};
        PlatformHandoff::Contents contents{.actions = std::move(platform.actions),
                                           .actionCapabilities = actionCapabilities,
                                           .affinityLayout = std::move(affinityLayout),
                                           .udpCollector = std::move(platform.udpCollector),
                                           .icons = std::move(platform.icons),
                                           .openFiles = std::move(platform.openFiles),
                                           .modules = std::move(platform.modules),
                                           .findCapabilities = findCapabilities};
        {
            const std::scoped_lock lock(handoff->mutex);
            handoff->contents = std::move(contents);
        }
        handoff->seeded.store(true, std::memory_order_release);
        spdlog::info("ProcessesPanel: process probe built and model seeded on the sampler thread");
    };
    m_Sampler = std::make_unique<Domain::BackgroundSampler>(samplerCfg);
    m_Sampler->addSamplable(m_ProcessModel, "processes");
    m_Sampler->start();

    // The saved columns, handed to the table on its first frame as a Columns menu request is: ShellLayer
    // restores the saved ImGui layout after this, and its visibility would otherwise win over
    // DefaultHide. A column this system cannot fill is hidden by default, unless its visibility was
    // chosen (#1210): that waits for the first generation, which brings the probe's capabilities
    // (applyStartupColumnDefaults()), so the table never shows a set computed without them.
    m_ColumnDefaultsApplied = false;
    m_RequestedColumns = m_ColumnSettings;
    m_TableShowsColumnSettings = false;

    spdlog::info("ProcessesPanel: initialized with background sampler ({}ms interval)", m_RefreshInterval.count());
}

bool ProcessesPanel::waitForFirstSample(std::chrono::milliseconds timeout)
{
    if (!m_Handoff)
    {
        return false; // not attached
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!m_Handoff->seeded.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    adoptPlatform();
    adoptNewerSnapshots();
    return true;
}

std::optional<ProcessesPanel::PlatformHandoff::Contents> ProcessesPanel::takePlatformHandoff()
{
    if (!m_Handoff)
    {
        return std::nullopt;
    }
    const std::scoped_lock lock(m_Handoff->mutex);
    return std::exchange(m_Handoff->contents, std::nullopt);
}

void ProcessesPanel::adoptPlatform()
{
    std::optional<PlatformHandoff::Contents> contents = takePlatformHandoff();
    if (!contents.has_value())
    {
        return;
    }
    // The row menu's actions (#1209), with what this platform can do, so it offers nothing it can't.
    m_ProcessActions = std::move(contents->actions);
    m_ActionCapabilities = contents->actionCapabilities;
    m_AffinityLayout = std::move(contents->affinityLayout);
    // The UDP event trace's collector (#1492), started by the hook if Settings had it on; the toggle
    // and detach go through this panel from here.
    m_UdpCollector = std::move(contents->udpCollector);
    // The icons beside the names (#1800), loaded on their own worker as rows are first drawn.
    m_IconView.attach(std::move(contents->icons));
    // Find handles (#3067): its readers, searched on the dialog's own worker.
    m_FindHandlesDialog.setReaders(std::move(contents->openFiles), std::move(contents->modules), contents->findCapabilities);
}

void ProcessesPanel::applyStartupColumnDefaults()
{
    // A column this system cannot fill is hidden by default, unless its visibility was chosen (#1210),
    // from the capabilities the first generation published. Later changes go through renderContent()'s
    // check against m_ColumnDefaultsCapabilities (#1254).
    m_ColumnDefaultsCapabilities = processCapabilities();
    m_GpuSupport = gpuSupport();
    m_ColumnDefaultsGpuSupport = m_GpuSupport;
    // Through the request path, as a Columns menu change is: the table may have drawn the saved
    // columns already, and the request is what it takes its next layout from.
    ProcessColumnSettings columns = m_RequestedColumns.value_or(m_ColumnSettings);
    ProcessColumnAvailability::applyCapabilityDefaults(columns, m_ColumnDefaultsCapabilities, m_GpuSupport);
    m_RequestedColumns = columns; // trivially copyable
    m_ColumnDefaultsApplied = true;
}

void ProcessesPanel::setSamplingInterval(std::chrono::milliseconds interval, bool forceSample)
{
    if (interval == m_RefreshInterval)
    {
        return;
    }
    m_RefreshInterval = interval;
    if (m_Sampler)
    {
        m_Sampler->setInterval(interval);
    }
    m_ForceRefresh = m_ForceRefresh || forceSample;
}

std::chrono::milliseconds ProcessesPanel::samplingInterval() const
{
    return m_Sampler ? m_Sampler->interval() : m_RefreshInterval;
}

void ProcessesPanel::requestRefresh()
{
    m_ForceRefresh = true;
}

void ProcessesPanel::onDetach()
{
    // Save column settings to user config
    UserConfig::get().settings().processColumns = m_ColumnSettings;
    UserConfig::get().settings().processMeters = m_MeterSettings;
    // Order doesn't matter for safety here, same as the destructor above: BackgroundSampler
    // observes m_ProcessModel via a weak_ptr, so it's never left holding a dangling pointer
    // regardless of which is reset first. Stopping the sampler first still avoids a sample()
    // call racing the rest of this teardown, and waits for its start hook if that is still building
    // the platform (#2731).
    if (m_Sampler)
    {
        m_Sampler->stop();
        m_Sampler.reset();
    }
    // A platform the hook handed over that no frame adopted (detached straight after attach): its
    // collector is stopped and its actions released below like adopted ones; its icons are dropped
    // unattached rather than attached to a view about to be detached.
    if (std::optional<PlatformHandoff::Contents> contents = takePlatformHandoff(); contents.has_value())
    {
        m_ProcessActions = std::move(contents->actions);
        m_UdpCollector = std::move(contents->udpCollector);
    }
    m_Handoff.reset();
    // While the GL context is current: deletes the icons' textures, and stops their loader (#1800).
    m_IconView.detach();
    // A dump still being written is cancelled and waited for, while its actions are still here (#807).
    m_DumpView.shutdown();
    // So is a batch still running (#2142): stopped before its next process.
    m_BatchRun.shutdown();
    // And a single-process action (#2305): one platform call, waited for while its actions are here --
    // asked to stop first, so a password prompt still up is given up on, not waited out (#2388).
    m_RowActionRun.requestStop();
    m_RowActionRun.update(/*wait=*/true);
    // The UDP event trace's session is stopped here, not left to whichever of the model and this
    // panel lets the collector go last (#1492): after a switch still running on its worker (#3155),
    // which is waited for, and whatever was asked for after it dropped. The one wait a detach makes
    // for the trace: nothing draws after it.
    logUdpSwitch(m_UdpSwitch.shutdown());
    if (m_UdpCollector)
    {
        m_UdpCollector->stop();
    }
    m_UdpCollector.reset();
    m_ProcessModel.reset();
}

void ProcessesPanel::applyUdpEventTrace(bool enabled)
{
    // Started or stopped on the switch's worker, never here (#3155): a start's stale-session sweep and
    // a stop's join of the consumer thread can take long. Null before the platform is adopted: the
    // start hook read the setting itself.
    logUdpSwitch(m_UdpSwitch.request(m_UdpCollector, enabled));
}

void ProcessesPanel::logUdpSwitch(const std::optional<UdpTraceSwitch::Outcome>& outcome)
{
    // A start the OS refused is logged by the collector, with why; only a switch that threw or had no
    // thread to run on is logged here.
    if (outcome.has_value() && !outcome->error.empty())
    {
        spdlog::warn("ProcessesPanel: couldn't {} the UDP event trace: {}", outcome->enabled ? "start" : "stop", outcome->error);
    }
}

void ProcessesPanel::onEvent(Core::Event& event)
{
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::ActiveTabChangedEvent>(
        [this](Core::ActiveTabChangedEvent& e)
        {
            // Only the drawing follows the tab: the sampler runs at the refresh setting whichever tab
            // is shown (#2731), so there is no relaxed cadence to catch up from on coming back.
            m_IsActiveTab = (e.tabName() == "Processes");
            // A freeze belongs to the pane being looked at; leaving it must not leave it frozen (#928).
            m_DisplayFreeze.reset();
            return false;
        });
    dispatcher.dispatch<Core::RefreshRateChangedEvent>(
        [this](Core::RefreshRateChangedEvent& e)
        {
            // The startup value (#1079) is applied without forcing a sample right after the seed (#1102).
            setSamplingInterval(std::chrono::milliseconds(e.getIntervalMs()), !e.isInitial());
            return false;
        });
    // The Settings toggle for the UDP event trace (#1492).
    dispatcher.dispatch<Core::UdpEventTraceChangedEvent>(
        [this](Core::UdpEventTraceChangedEvent& e)
        {
            applyUdpEventTrace(e.isEnabled());
            return false;
        });
    // This panel owns the process model, so it sets the model's history length (#1078).
    dispatcher.dispatch<Core::HistoryDurationChangedEvent>(
        [this](Core::HistoryDurationChangedEvent& e)
        {
            if (m_ProcessModel)
            {
                m_ProcessModel->setMaxHistorySeconds(Domain::Numeric::toDouble(e.getSeconds()));
            }
            return false;
        });
    // A theme or font-size change needs no handling here (#1178): TextSizeCache::isValid() notices a
    // new font (or a rebuilt atlas) on the next frame and remeasures, which also restamps the row
    // format cache, and colours are read from the theme every frame. Neither needs new process data.
}

void ProcessesPanel::onUpdate(float deltaTime)
{
    // The UDP event trace's switch (#3155): a finished one taken in, and one asked for meanwhile started.
    logUdpSwitch(m_UdpSwitch.update());
    if (m_RowActionResultSeconds > 0.0F)
    {
        m_RowActionResultSeconds = std::max(0.0F, m_RowActionResultSeconds - std::max(0.0F, deltaTime));
    }
    // A batch confirmed on an earlier frame has finished on its worker (#2142): its one line replaces
    // the progress line.
    if (std::optional<ProcessBatch::BatchRunOutcome> outcome = m_BatchRun.poll(); outcome.has_value())
    {
        showBatchOutcome(std::move(*outcome));
    }
    // So has a single-process action (#2305): its result line replaces "Killing PID 123...".
    m_RowActionRun.update();

    if (!m_ProcessModel || !m_Sampler)
    {
        return;
    }

    // The sampler's interval is not touched here: it is the refresh setting, set by setSamplingInterval()
    // alone, whatever the frame rate and whether the window is being resized, moved or minimized (#2731).
    // Sampling and drawing are separate.

    // Force-refresh: ask the background sampler to run an extra enumeration immediately.
    if (m_ForceRefresh)
    {
        m_Sampler->requestRefresh();
        m_ForceRefresh = false;
    }

    adoptPlatform(); // the actions, icons and UDP collector the sampler thread built (#2731), once
    adoptNewerSnapshots();
    // The test hook's startup selection (#1559), and a demo script's (#2827): one flag test per frame
    // each until it fires or gives up.
    if (m_StartupSelection.pending())
    {
        applyPendingSelection(m_StartupSelection, m_StartupSelectionShowsDetails, "startup selection");
    }
    if (m_RequestedSelection.pending())
    {
        applyPendingSelection(m_RequestedSelection, false, "requested selection");
    }
}

void ProcessesPanel::requestStartupSelection(std::optional<SelectOverride::Target> target, const bool showDetails)
{
    m_StartupSelectionShowsDetails = showDetails;
    m_StartupSelection = SelectOverride::Pending(std::move(target));
}

void ProcessesPanel::requestSelection(SelectOverride::Target target)
{
    m_RequestedSelection = SelectOverride::Pending(std::move(target));
}

void ProcessesPanel::applyPendingSelection(SelectOverride::Pending& pending, const bool showDetails, const std::string_view what)
{
    const std::vector<Domain::ProcessSnapshot>& snapshots = *m_CachedRenderSnapshots;
    const SelectOverride::Pending::Step step = pending.onSnapshot(m_CachedSnapshotVersion, snapshots);
    if (step.match.has_value())
    {
        // As a plain click on the row, then (showDetails) the row menu's Details (#1209).
        const ProcessSelection::Identity id = ProcessSelection::identityOf(snapshots[*step.match]);
        spdlog::info("ProcessesPanel: {} found PID {} ({})", what, id.pid, snapshots[*step.match].name);
        m_Selection.selectOnly(id);
        selectProcess(id);
        if (showDetails)
        {
            Core::ShowProcessDetailsEvent event;
            raiseEvent(event);
        }
    }
    else if (step.gaveUp)
    {
        spdlog::warn("ProcessesPanel: {}: no process matched after {} snapshots; nothing selected", what, SelectOverride::MAX_SNAPSHOTS);
    }
}

void ProcessesPanel::adoptNewerSnapshots()
{
    // Held Ctrl (#928): keep the adopted generation, so the filter, sort and row caches keyed on it
    // stay as they are. The model keeps sampling; releasing Ctrl adopts its latest generation.
    if (m_DisplayFreeze.frozen() && m_CachedSnapshotVersion != std::numeric_limits<std::uint64_t>::max())
    {
        return;
    }
    // Detect and copy new data in a single lock acquisition. tryCopySnapshotsIfNewer() checks the
    // published version lock-free first, so a call with nothing new costs one atomic load.
    std::uint64_t newVersion = m_CachedSnapshotVersion;
    if (m_ProcessModel->tryCopySnapshotsIfNewer(
            m_CachedSnapshotVersion, m_CachedRenderSnapshots, newVersion, &m_CachedCapabilities, &m_CachedGpuSupport, &m_CachedRowHistory))
    {
        m_CachedSnapshotVersion = newVersion;
        // Which processes exited since the generation before, kept as dimmed rows for a moment (#3060).
        if (UserConfig::get().settings().highlightNewProcesses && m_PreviousGeneration)
        {
            (void) m_ExitingRows.onGeneration(*m_PreviousGeneration, *m_CachedRenderSnapshots, Core::Application::getTime());
        }
        m_PreviousGeneration = m_CachedRenderSnapshots;
        // Processes stuck in uninterruptible sleep, tinted (#3097).
        m_StuckProcesses.onGeneration(*m_CachedRenderSnapshots, Core::Application::getTime());
        // Selected processes that have exited drop out of the selection (#804): once per generation,
        // one O(1) lookup per process. The primary process stays, so Process Details can show it exited.
        (void) m_Selection.retainPresent(*m_CachedRenderSnapshots);
        // The first generation brings the probe's capabilities, which the column defaults wait for (#2731).
        if (!m_ColumnDefaultsApplied)
        {
            applyStartupColumnDefaults();
        }
    }
}

void ProcessesPanel::updateDisplayFreeze()
{
    const ImGuiIO& io = ImGui::GetIO();
    const ProcessDisplayFreeze::Inputs inputs{
        .appFocused = !io.AppFocusLost,
        .ctrlHeld = io.KeyCtrl,
        .otherModifierHeld = io.KeyShift || io.KeyAlt || io.KeySuper,
        .otherKeyHeld = anyNonModifierKeyDown(),
        .textInputActive = io.WantTextInput,
        .panelHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows),
        .panelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows),
    };
    const bool wasFrozen = m_DisplayFreeze.frozen();
    if (m_DisplayFreeze.update(inputs, Core::Application::getTime()) != wasFrozen)
    {
        spdlog::debug("ProcessesPanel: display {} (Ctrl)", wasFrozen ? "resumed" : "frozen");
    }
}

void ProcessesPanel::updateDisplaySnapshots()
{
    // Exited rows leave after ExitingProcessRows::DURATION_SECONDS, or all at once when the setting is off.
    const bool dropped =
        UserConfig::get().settings().highlightNewProcesses ? m_ExitingRows.expire(Core::Application::getTime()) : m_ExitingRows.clear();
    if (!dropped && m_DisplayBuiltForVersion == m_CachedSnapshotVersion)
    {
        return;
    }
    m_DisplayBuiltForVersion = m_CachedSnapshotVersion;
    m_LiveRowCount = m_CachedRenderSnapshots->size();
    // With nothing exited, the shared generation itself: no copy.
    m_DisplaySnapshots = m_ExitingRows.exited().empty()
                           ? m_CachedRenderSnapshots
                           : std::make_shared<const std::vector<Domain::ProcessSnapshot>>(
                                 ExitingProcessRows::displayRows(*m_CachedRenderSnapshots, m_ExitingRows.exited()));
    ++m_DisplayVersion;
}

void ProcessesPanel::renderContent()
{
    // An F9 asked for on a frame the table is not drawn is dropped, never kept for a later one (#170).
    const bool killRequested = m_KillShortcut.take();

    if (!m_ProcessModel)
    {
        UI::Widgets::renderEmptyState(ICON_FA_TRIANGLE_EXCLAMATION "  Process list unavailable",
                                      "The process model is not initialized, so no processes can be listed.");
        return;
    }

    // Skip rendering when tab is inactive (data collection continues in onUpdate)
    if (!m_IsActiveTab)
    {
        return;
    }

    // Ensure text size cache is valid for current font (called once per frame)
    ensureTextSizeCacheValid();

    // Decide the Ctrl freeze before adopting, so a frozen frame keeps the generation it shows (#928).
    updateDisplayFreeze();

    // Keyboard navigation (#160), read here in the panel's window: while it is hovered or focused and
    // nothing is typed or open, the navigation keys are taken from ImGui's own keyboard navigation and
    // move the selection instead. Applied inside the table, once the visible order is known.
    auto navCommand = ProcessTableNavigation::NavCommand::None;
    bool selectAllRequested = false;
    if (KeyboardInput::tableNavigationArmed())
    {
        KeyboardInput::claimNavigationKeys(ImGui::GetID("##ProcessTableKeys"), m_TreeViewEnabled);
        navCommand = KeyboardInput::pollNavigationCommand(m_TreeViewEnabled);
        selectAllRequested = KeyboardInput::pollSelectAll(); // Ctrl+A (#804)
        if (KeyboardInput::pollIncrementalSearch())          // "/" (#3097): the Find box, empty, with the keyboard
        {
            m_IncrementalSearchOpen = true;
            m_IncrementalSearchFocus = true;
            m_IncrementalSearchMissed = false;
            m_IncrementalSearchText.clear();
        }
        if (KeyboardInput::pollFocusFilter()) // Ctrl+F: the filter box takes the keyboard
        {
            m_FocusFilter = true;
        }
        if (KeyboardInput::pollFindHandles() && m_FindHandlesDialog.available()) // Ctrl+Shift+F (#3067)
        {
            m_FindHandlesDialog.open();
        }
    }

    // Get thread-safe copy of snapshots — only when data has actually changed (version-cached).
    // ProcessModel updates at 1Hz but render runs at 60fps; skip 59/60 redundant deep copies.
    // onUpdate() adopts too, but onUpdate() is skipped without a sampler and a generation can be
    // published between the two; with nothing new this is one atomic load. Everything below is keyed
    // on m_CachedSnapshotVersion, the generation adopted with the vector, not on a version read
    // before adopting (#1394).
    adoptPlatform();
    adoptNewerSnapshots();
    updateDisplaySnapshots();
    // The rows drawn: the adopted generation, then any process that exited a moment ago (#3060). The
    // filter, sort and tree caches are keyed on m_DisplayVersion, which changes with either.
    const std::uint64_t adoptedVersion = m_DisplayVersion;
    const auto& liveSnapshots = *m_CachedRenderSnapshots;
    const auto& currentSnapshots = *m_DisplaySnapshots;

    // The probe's capabilities can change mid-run (#1254): columns whose visibility was not chosen
    // follow them, through the same request path as the Columns menu (#1210).
    // Per-process GPU support comes from the GPU probe, known only once it has started (#1210), as is
    // whether an accelerator is listed, which shows NPU % by default unless the user chose it (#1870).
    m_GpuSupport = gpuSupport();
    if (const Platform::ProcessCapabilities caps = processCapabilities();
        caps != m_ColumnDefaultsCapabilities || m_GpuSupport != m_ColumnDefaultsGpuSupport)
    {
        // The cached cell texts were formatted for the old capabilities (a GPU cell as a measured
        // value or as "not available"), and a cache entry is otherwise rebuilt only for a new process
        // generation; GPU support comes from another model, so it can change between generations.
        m_RowFormatCache.clear();
        m_ColumnDefaultsCapabilities = caps;
        m_ColumnDefaultsGpuSupport = m_GpuSupport;
        if (auto changed =
                ProcessColumnAvailability::capabilityDefaultChanges(m_RequestedColumns.value_or(m_ColumnSettings), caps, m_GpuSupport))
        {
            m_RequestedColumns = *changed;
        }
    }

    // Prune row format cache entries for processes no longer present, once per new snapshot
    // generation (not per frame). Entries themselves are built lazily, on demand, by
    // renderProcessRow() -- see m_RowFormatCache's doc comment -- so this pass only bounds the
    // map's size; it does not rebuild anything. Live keys are collected into a set first so the
    // prune is O(n) overall rather than O(cache size * process count).
    if (m_CachedSnapshotVersion != m_RowFormatCachePrunedVersion)
    {
        std::unordered_set<std::uint64_t> liveKeys;
        liveKeys.reserve(currentSnapshots.size());
        for (const auto& proc : currentSnapshots)
        {
            liveKeys.insert(proc.uniqueKey);
        }
        std::erase_if(m_RowFormatCache, [&liveKeys](const auto& entry) { return !liveKeys.contains(entry.first); });
        // And the tree's collapsed nodes that have exited (#838). A uniqueKey is never reused, so one
        // gone from the snapshot can never be drawn again.
        const bool prunedCollapsed = ProcessTreeFlatten::pruneCollapsedKeys(m_CollapsedKeys, liveKeys);
        if (ProcessTreeFlatten::pruneCollapsedKeys(m_FilterCollapsedKeys, liveKeys) || prunedCollapsed)
        {
            ++m_CollapseGeneration;
        }
        m_RowFormatCachePrunedVersion = m_CachedSnapshotVersion;
    }

    // Search bar
    const auto& theme = UI::Theme::get();
    // Sized from the font and the hint it has to show, not a fixed 200px (#965).
    ImGui::SetNextItemWidth(ProcessTableLayout::computeFilterWidth(
        ImGui::CalcTextSize(FILTER_HINT).x, ImGui::GetStyle().FramePadding.x, ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x));
    // The hint in the muted text colour: the running-process green read as a status (#1196).
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme.scheme().textMuted);

    {
        const UI::Widgets::FrameOutlineScope frameOutline;
        if (std::exchange(m_FocusFilter, false))
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputTextWithHint("##search", FILTER_HINT, &m_SearchBuffer);
    }
    ImGui::PopStyleColor();

    // Clear button
    ImGui::SameLine();
    if (!m_SearchBuffer.empty())
    {
        if (ImGui::SmallButton(ICON_FA_XMARK))
        {
            m_SearchBuffer.clear();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Clear filter");
        }
    }
    renderIncrementalSearchBox();

    // Filter snapshots based on search — cached to avoid O(n) rebuild every 60fps frame.
    // Filtered indices, running count, and summary string are only recomputed when snapshot
    // version or search term changes (typically once per second at the 1Hz refresh rate).
    const std::string_view searchTerm(m_SearchBuffer);
    // A pin added or removed (#3061) refilters too: a pinned process stays through the filter.
    const bool filterDirty = ProcessFilterCache::isStale(adoptedVersion, m_CachedFilterVersion, searchTerm, m_CachedSearchTerm) ||
                             std::exchange(m_PinsChanged, false);
    if (filterDirty)
    {
        m_CachedFilteredIndices.clear();
        m_CachedFilteredIndices.reserve(currentSnapshots.size());

        // Case-insensitive, in the name or the command line, as the user guide says (#838)
        for (size_t i = 0; i < currentSnapshots.size(); ++i)
        {
            if (ProcessFilterCache::matches(currentSnapshots[i].name, currentSnapshots[i].command, searchTerm) ||
                m_Pins.contains(currentSnapshots[i].name)) // a pinned process stays in view (#3061)
            {
                m_CachedFilteredIndices.push_back(i);
            }
        }
        if (searchTerm.empty())
        {
            m_FilterCollapsedKeys.clear(); // The next filter opens fully expanded (#838)
        }

        // Counted from the running processes only: an exited row is not one (#3060).
        m_CachedRunningCount = 0;
        for (const auto& proc : liveSnapshots)
        {
            if (proc.displayState == "Running")
            {
                ++m_CachedRunningCount;
            }
        }

        if (searchTerm.empty())
        {
            m_CachedSummaryStr = std::format("{} processes, {} running",
                                             UI::Format::formatIntLocalized(liveSnapshots.size()),
                                             UI::Format::formatIntLocalized(m_CachedRunningCount));
        }
        else
        {
            m_CachedSummaryStr =
                std::format("{} / {} processes",
                            UI::Format::formatIntLocalized(ExitingProcessRows::countLive(m_CachedFilteredIndices, m_LiveRowCount)),
                            UI::Format::formatIntLocalized(liveSnapshots.size()));
        }

        m_CachedFilterVersion = adoptedVersion;
        m_CachedSearchTerm = std::string(searchTerm);
        ++m_FilterGeneration; // The tree's rows are rebuilt from the new indices (#1138)

        // Reset sorted indices to natural order; the sort below sorts them again this frame (#1174).
        // Pinned processes first (#3061), which stands when the table has no sort.
        m_CachedSortedIndices = m_CachedFilteredIndices;
        ProcessPins::pinnedFirst(m_CachedSortedIndices, currentSnapshots, m_Pins);
        m_SortPending = true;
    }

    // Process count with state summary (filtered/total), or for a few seconds the result of an action
    // taken from a row's menu (#1209). Then the Columns button and the List | Tree control.
    ImGui::SameLine();

    // Fixed widths from cached label widths, so the row does not shift when the mode changes.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float columnsButtonWidth = m_TextSizeCache.columnsLabelWidth + (style.FramePadding.x * 2.0F);
    const float listSegmentWidth = m_TextSizeCache.listViewLabelWidth + (style.FramePadding.x * 2.0F);
    const float treeSegmentWidth = m_TextSizeCache.treeViewLabelWidth + (style.FramePadding.x * 2.0F);
    // In tree view, Expand all and Collapse all follow the List | Tree control (#838).
    const float foldButtonWidth = m_TextSizeCache.foldIconWidth + (style.FramePadding.x * 2.0F);
    const float foldButtonsWidth = m_TreeViewEnabled ? (style.ItemSpacing.x + foldButtonWidth) * 2.0F : 0.0F;
    // "Authenticate and retry" beside a refused action's line (#2308) takes its room from the status.
    const bool offerRetry = offersAuthenticateAndRetry();
    const float retryButtonWidth = offerRetry ? ImGui::CalcTextSize(AuthorizeWording::RETRY_LABEL).x + (style.FramePadding.x * 2.0F) : 0.0F;
    // Group by (#3068): "Group: None", sized for its widest label so the row doesn't shift.
    const float groupComboWidth = ImGui::CalcTextSize("Group: Container").x + ImGui::GetFrameHeight() + (style.FramePadding.x * 2.0F);
    // Run new task (#3065), an icon button before Columns, where the platform can start programs.
    const bool offerRunTask = m_ActionCapabilities.canRunProgram;
    const float runTaskButtonWidth = offerRunTask ? ImGui::CalcTextSize(ICON_FA_ROCKET).x + (style.FramePadding.x * 2.0F) : 0.0F;
    // Find handles (#3067), an icon button beside it, where this run can read processes' files.
    const bool offerFindHandles = m_FindHandlesDialog.available();
    const float findButtonWidth = offerFindHandles ? ImGui::CalcTextSize(ICON_FA_MAGNIFYING_GLASS).x + (style.FramePadding.x * 2.0F) : 0.0F;
    const float controlsWidth = groupComboWidth + style.ItemSpacing.x + (offerRunTask ? runTaskButtonWidth + style.ItemSpacing.x : 0.0F) +
                                (offerFindHandles ? findButtonWidth + style.ItemSpacing.x : 0.0F) + columnsButtonWidth +
                                style.ItemSpacing.x + listSegmentWidth + treeSegmentWidth + foldButtonsWidth +
                                (offerRetry ? retryButtonWidth + style.ItemSpacing.x : 0.0F);

    // An action's result takes no more room than the count text it stands in for, ellipsized with
    // the full text as its tooltip, so a long platform error cannot push the controls off-screen.
    // While a batch runs (#2142), its progress stands there instead, uncoloured.
    const bool batchRunning = rowActionRunning();
    const bool showActionResult = !batchRunning && (m_RowActionResultSeconds > 0.0F) && !m_RowActionResult.empty();
    const std::string* statusSource = &m_CachedSummaryStr;
    if (batchRunning)
    {
        statusSource = m_BatchRun.busy() ? &m_BatchRun.statusText() : &m_RowActionRun.pendingText();
    }
    else if (showActionResult)
    {
        statusSource = &m_RowActionResult.text;
    }
    const std::string& statusText = *statusSource;
    const float rightEdgeX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float summaryW = ImGui::CalcTextSize(m_CachedSummaryStr.c_str(), m_CachedSummaryStr.c_str() + m_CachedSummaryStr.size()).x;
    const float textW =
        (batchRunning || showActionResult) ? ImGui::CalcTextSize(statusText.c_str(), statusText.c_str() + statusText.size()).x : summaryW;
    const float statusMinX = ImGui::GetCursorPosX();
    const ProcessTableLayout::ToolbarStatusLayout statusLayout =
        ProcessTableLayout::layoutToolbarStatus(statusMinX, rightEdgeX, controlsWidth, style.ItemSpacing.x, textW, summaryW);
    // While Ctrl freezes the pane (#928) the paused label sits left of the status text, so the text
    // and the controls keep their places. On a row too narrow for the full label it shrinks to its icon.
    auto pausedForm = ProcessTableLayout::PausedLabelForm::Hidden;
    if (m_DisplayFreeze.frozen())
    {
        pausedForm = ProcessTableLayout::choosePausedLabelForm(statusLayout.x - statusMinX,
                                                               ImGui::CalcTextSize(FROZEN_LABEL).x + style.ItemSpacing.x,
                                                               ImGui::CalcTextSize(FROZEN_ICON).x + style.ItemSpacing.x);
    }
    const char* pausedText = nullptr;
    if (pausedForm == ProcessTableLayout::PausedLabelForm::Full)
    {
        pausedText = FROZEN_LABEL;
    }
    else if (pausedForm == ProcessTableLayout::PausedLabelForm::IconOnly)
    {
        pausedText = FROZEN_ICON;
    }
    if (pausedText != nullptr)
    {
        const float pausedW = ImGui::CalcTextSize(pausedText).x + style.ItemSpacing.x;
        ImGui::SetCursorPosX(std::max(statusMinX, statusLayout.x - pausedW));
        ImGui::TextColored(theme.scheme().textWarning, "%s", pausedText);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Updates are paused while Ctrl is held.\nSampling continues; release Ctrl to resume.");
        }
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(statusLayout.x);
    {
        const bool colored = showActionResult;
        if (colored)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, m_RowActionResult.ok ? theme.scheme().textSuccess : theme.scheme().textError);
        }
        renderBoundedText(statusText, textW, statusLayout.width);
        if (colored)
        {
            ImGui::PopStyleColor();
        }
    }
    if (offerRetry)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton(AuthorizeWording::RETRY_LABEL))
        {
            retryRowActionAuthorized();
        }
        ImGui::SetItemTooltip("%s", AuthorizeWording::RETRY_TOOLTIP);
    }

    // Group by (#3068): processes under group header rows with Σ totals.
    ImGui::SameLine();
    {
        ImGui::SetNextItemWidth(groupComboWidth);
        const std::string preview = std::format("Group: {}", ProcessGrouping::groupByLabel(m_GroupBy));
        const UI::Widgets::FrameOutlineScope frameOutline;
        if (ImGui::BeginCombo("##GroupBy", preview.c_str()))
        {
            for (const ProcessGrouping::GroupBy mode : {ProcessGrouping::GroupBy::None,
                                                        ProcessGrouping::GroupBy::Unit,
                                                        ProcessGrouping::GroupBy::Container,
                                                        ProcessGrouping::GroupBy::App})
            {
                const std::string label(ProcessGrouping::groupByLabel(mode));
                if (ImGui::Selectable(label.c_str(), mode == m_GroupBy))
                {
                    setGroupBy(mode);
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SetItemTooltip("Group processes by systemd unit, container or app, each group under a row with its totals");
    if (offerRunTask)
    {
        ImGui::SameLine();
        ImGui::BeginDisabled(rowActionRunning());
        if (ImGui::Button(ICON_FA_ROCKET "###RunNewTask", ImVec2(runTaskButtonWidth, 0.0F)))
        {
            m_RunTaskDialog.open(UserConfig::get().settings().recentCommands, m_ActionCapabilities.canRunProgramAsAdministrator);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Run new task: start a program, optionally as administrator");
    }
    if (offerFindHandles)
    {
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS "###FindHandles", ImVec2(findButtonWidth, 0.0F)))
        {
            m_FindHandlesDialog.open();
        }
        ImGui::SetItemTooltip("Find handles (Ctrl+Shift+F): which processes have a file, folder or library open");
    }

    // Column chooser, beside ImGui's own header right-click menu (#1209)
    ImGui::SameLine();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - constexpr literal
    if (ImGui::Button(COLUMNS_LABEL.data(), ImVec2(columnsButtonWidth, 0.0F)))
    {
        ImGui::OpenPopup(COLUMNS_POPUP_ID);
    }
    ImGui::SetItemTooltip("Show, hide or reset columns (also on a column header's right-click menu)");
    // Opens below the button, right edges aligned, rather than at the pointer, where it covered the
    // button that opened it.
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y), ImGuiCond_Appearing, ImVec2(1.0F, 0.0F));
    if (ImGui::BeginPopup(COLUMNS_POPUP_ID))
    {
        renderColumnsMenu();
        ImGui::EndPopup();
    }

    // View mode: List | Tree, the current one drawn selected (#1209)
    ImGui::SameLine();
    if (viewModeSegment(LIST_VIEW_LABEL, !m_TreeViewEnabled, listSegmentWidth))
    {
        setTreeView(false);
    }
    ImGui::SetItemTooltip("List view (F5): every process, sorted by the column you click");
    ImGui::SameLine(0.0F, 0.0F);
    if (viewModeSegment(TREE_VIEW_LABEL, m_TreeViewEnabled, treeSegmentWidth))
    {
        setTreeView(true);
    }
    ImGui::SetItemTooltip("Tree view (F5): processes under their parents, siblings sorted by the column you click");
    if (m_TreeViewEnabled)
    {
        renderTreeFoldButtons(currentSnapshots, foldButtonWidth);
    }

    // The row menu's batch priority dialog (#1484); a picked value goes on to the confirmation below.
    renderBatchPriorityDialog();
    // The row menu's Set affinity... (#1887), applied on its own Apply.
    renderAffinityDialog();
    renderRunTaskDialog();
    renderFindHandlesDialog(currentSnapshots);

    // A row menu's Suspend, Resume, Terminate or Kill, confirmed as in the Actions block (#1209), or a
    // batch priority change (#1484)
    renderRowActionConfirm();
    // The row menu's Create dump file... (#807): its confirm, then its progress and result.
    m_DumpView.render(m_ProcessActions.get());

    // The row menu (#1209), for the process m_RowMenuTarget holds. One popup at panel level rather
    // than one per row, opened by ID from the row (m_RowMenuPopupId, this same ID stack).
    m_RowMenuPopupId = ImGui::GetID(ROW_MENU_POPUP_ID);
    if (ImGui::BeginPopup(ROW_MENU_POPUP_ID))
    {
        if (m_RowMenuTarget.has_value())
        {
            renderRowContextMenu(*m_RowMenuTarget);
        }
        ImGui::EndPopup();
    }
    else
    {
        m_RowMenuTarget.reset();
    }

    // Always create all columns with stable IDs (using enum value as ID)
    // Hidden columns use ImGuiTableColumnFlags_Disabled
    const int totalColumns = UI::Format::checkedCount(processColumnCount());

    // Explicit outer_size keeps horizontal/vertical scroll extents aligned with the panel's current content region.
    // This adapts to whichever parent layout is active instead of assuming a fixed child height contract.
    const ImVec2 tableOuterSize = ImGui::GetContentRegionAvail();

    // Command absorbs whatever width the other columns leave, but never shrinks below its default;
    // past that point the table scrolls instead. See ProcessTableLayout.h for why this takes an
    // explicit inner width rather than just a stretch column (#924).
    const float emPx = ImGui::GetFontSize();
    // A hidden Command column reserves nothing: its minimum would otherwise be added back on top of
    // a measurement that already excludes it, giving a table that fits a scrollbar and an empty
    // scroll extent it does not need.
    const float commandMinWidth =
        m_ColumnSettings.isVisible(ProcessColumn::Command) ? scaledDefaultWidth(getColumnInfo(ProcessColumn::Command), emPx) : 0.0F;
    const float tableInnerWidth = ProcessTableLayout::computeInnerWidth(m_OtherColumnsWidth, commandMinWidth, m_TableVisibleWidth);

    if (ImGui::BeginTable("ProcessTable", totalColumns, ProcessTableFlags::forProcessTable(), tableOuterSize, tableInnerWidth))
    {
        ImGui::TableSetupScrollFreeze(0, 1); // Freeze header row

        // Setup ALL columns with stable IDs - use enum value as user_id for stable identification
        int commandColumnIdx = -1;
        int setupIdx = 0;
        for (const ProcessColumn col : allProcessColumns())
        {
            const auto info = getColumnInfo(col);
            ImGuiTableColumnFlags flags = ImGuiTableColumnFlags_None;
            if (col == ProcessColumn::Command)
            {
                commandColumnIdx = setupIdx;
            }
            ++setupIdx;

            // Set default visibility from settings (ImGui will manage the actual state)
            if (!m_ColumnSettings.isVisible(col))
            {
                flags |= ImGuiTableColumnFlags_DefaultHide;
            }

            // PID and Name columns cannot be hidden
            if (!info.canHide)
            {
                flags |= ImGuiTableColumnFlags_NoHide;
            }

            // Default sort on CPU%
            if (col == ProcessColumn::CpuPercent)
            {
                flags |= ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending;
            }

            // Command is the one stretch column: it takes the width the others leave. Its default
            // width is not an initial width here but the floor enforced through tableInnerWidth.
            //
            // It is also pinned as the trailing column. ImGui makes the right-most enabled column
            // non-resizable whenever the table has a stretch column, which is harmless while that
            // column is Command itself; were Command dragged elsewhere, whichever fixed column ended
            // up last would silently lose its resize handle. NoReorder keeps Command in place and
            // stops other columns crossing over it.
            if (col == ProcessColumn::Command)
            {
                flags |= ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoReorder;
                ImGui::TableSetupColumn(std::string(info.menuName).c_str(), flags, 1.0F, toImGuiId(col));
            }
            // Columns with a positive default width are initialized as width-based columns.
            else if (info.defaultWidth > 0.0F)
            {
                // Use menuName for TableSetupColumn (shown in context menu)
                // We render custom headers with info.name below
                // The default is authored at the reference font; scale it to the current one (#913).
                // Priority starts wide enough for its longest label, measured in the current font (#1280).
                const float defaultWidth =
                    (col == ProcessColumn::Priority)
                        ? contentFittedWidth(scaledDefaultWidth(info, emPx), m_TextSizeCache.cells.widestPriorityLabel, emPx)
                        : scaledDefaultWidth(info, emPx);
                ImGui::TableSetupColumn(std::string(info.menuName).c_str(), flags, defaultWidth, toImGuiId(col));
            }
            else
            {
                flags |= ImGuiTableColumnFlags_WidthStretch;
                ImGui::TableSetupColumn(std::string(info.menuName).c_str(), flags, 0.0F, toImGuiId(col));
            }
        }

        // The toolbar's Columns menu and a view-mode change, while the layout can still change (#1209)
        const bool columnsChangedByMenu = applyColumnRequests();
        syncNameWidthForViewMode();

        // Headers are aligned like their cells: a numeric header over its right-aligned numbers (#1209)
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        int headerIdx = 0;
        const ImGuiStyle& headerStyle = ImGui::GetStyle();
        const Platform::ProcessCapabilities headerCaps = processCapabilities();
        const float sortArrowReserve =
            ProcessTableLayout::sortArrowReserve(ImGui::GetFontSize(), headerStyle.FramePadding.x, headerStyle.CellPadding.x);
        for (const ProcessColumn col : allProcessColumns())
        {
            if (!ImGui::TableSetColumnIndex(headerIdx))
            {
                ++headerIdx;
                continue;
            }

            const auto info = getColumnInfo(col);
            // ImGui draws the sort arrow at the sorted header's right edge, in either view (#838).
            const bool showsSortArrow = (ImGui::TableGetColumnFlags(headerIdx) & ImGuiTableColumnFlags_IsSorted) != 0;
            // The header item keeps the whole cell as its sort and right-click target: it is submitted
            // at the cell's start with a hidden label, and the name is drawn at its aligned position
            // over it. Moving the cursor before TableHeader() moved the item's start too, so an
            // aligned header lost its blank left part as a click target (#1365 review).
            const ImVec2 cellStart = ImGui::GetCursorScreenPos();
            const float cellWidth = ImGui::GetContentRegionAvail().x;
            const float labelOffset = ProcessTableLayout::headerLabelOffset(
                columnAlignment(col), cellWidth, m_TextSizeCache.getHeaderWidth(col), showsSortArrow ? sortArrowReserve : 0.0F);
            // A column this system cannot fill is headed muted, like its cells (#1210).
            const bool columnSupported = ProcessColumnAvailability::isSupported(col, headerCaps, m_GpuSupport);
            {
                const MutedCellScope muted(!columnSupported);
                ImGui::PushID(static_cast<int>(col));
                ImGui::TableHeader("##header");
                ImGui::PopID();
                // TableHeader() draws its label at the cell's cursor position; this draws ours there,
                // shifted by the alignment offset, in the (possibly muted) text colour. Ellipsized,
                // as TableHeader()'s own label is, short of the sort arrow it draws at the right
                // edge, so a narrow sorted column's name never covers its arrow (#1365 review).
                const float labelRight = cellStart.x + std::max(0.0F, cellWidth - (showsSortArrow ? sortArrowReserve : 0.0F));
                ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(),
                                          ImVec2(cellStart.x + labelOffset, cellStart.y),
                                          ImVec2(labelRight, cellStart.y + ImGui::GetTextLineHeight()),
                                          labelRight,
                                          info.name.data(),
                                          info.name.data() + info.name.size(),
                                          nullptr);
            }

            // Show tooltip with full column name and description on hover, and what the column's
            // missing values mean (#1210). Every line is a constexpr literal: nothing is formatted.
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && ImGui::BeginTooltip())
            {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * HEADER_TOOLTIP_WRAP_EM);
                const auto line = [](std::string_view text)
                {
                    if (!text.empty())
                    {
                        ImGui::TextUnformatted(text.data(), text.data() + text.size());
                    }
                };
                line(info.menuName);
                line(info.description);
                // The TCP-only note says nothing more for a column that has no values at all.
                if (columnSupported)
                {
                    line(columnCapabilityNote(col, headerCaps.hasUdpNetworkCounters));
                }
                line(ProcessColumnAvailability::unavailableValuesNote(col, headerCaps, m_GpuSupport));
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            ++headerIdx;
        }

        // Measure this frame's layout for the next frame's inner-width decision. The layout is
        // locked once the header row has been submitted, so these are final for the frame.
        if (const ImGuiTable* table = ImGui::GetCurrentTable(); table != nullptr)
        {
            m_TableId = table->ID;
            m_TableHasDefaultOrder = table->IsDefaultDisplayOrder;
        }
        if (const ImGuiTable* table = ImGui::GetCurrentTable(); table != nullptr && commandColumnIdx >= 0)
        {
            const ImGuiTableColumn& commandColumn = table->Columns[commandColumnIdx];
            const float commandWidth = commandColumn.IsEnabled ? commandColumn.WidthGiven : 0.0F;
            m_OtherColumnsWidth = table->ColumnsGivenWidth - commandWidth;
            m_TableVisibleWidth = table->InnerClipRect.GetWidth();
        }

        // F6's "Sort by" menu (#3097): the visible sortable columns, the sort column ticked. Picking it
        // again reverses the order; another column sorts by it in that column's own first direction.
        // Applied below the menu, so the sort below picks it up this frame.
        if (m_SortMenuShortcut.take())
        {
            ImGui::OpenPopup(SORT_MENU_POPUP_ID);
        }
        std::optional<int> sortPick;
        if (ImGui::IsPopupOpen(SORT_MENU_POPUP_ID))
        {
            // Read here, in the table: inside the popup's own window ImGui has no current table.
            struct SortChoice
            {
                int column;
                const char* name;
                bool sorted;
            };
            std::vector<SortChoice> choices;
            for (int column = 0; column < ImGui::TableGetColumnCount(); ++column)
            {
                const ImGuiTableColumnFlags flags = ImGui::TableGetColumnFlags(column);
                if ((flags & ImGuiTableColumnFlags_IsEnabled) != 0 && (flags & ImGuiTableColumnFlags_NoSort) == 0)
                {
                    choices.push_back({.column = column,
                                       .name = ImGui::TableGetColumnName(column),
                                       .sorted = (flags & ImGuiTableColumnFlags_IsSorted) != 0});
                }
            }
            if (ImGui::BeginPopup(SORT_MENU_POPUP_ID))
            {
                ImGui::TextDisabled("Sort by");
                ImGui::Separator();
                for (const SortChoice& choice : choices)
                {
                    if (ImGui::MenuItem(choice.name, nullptr, choice.sorted))
                    {
                        sortPick = choice.column;
                    }
                }
                ImGui::EndPopup();
            }
        }
        if (sortPick.has_value())
        {
            const ImGuiTableColumnFlags flags = ImGui::TableGetColumnFlags(*sortPick);
            ImGuiSortDirection direction =
                (flags & ImGuiTableColumnFlags_PreferSortDescending) != 0 ? ImGuiSortDirection_Descending : ImGuiSortDirection_Ascending;
            if (const ImGuiTable* table = ImGui::GetCurrentTable(); table != nullptr && (flags & ImGuiTableColumnFlags_IsSorted) != 0)
            {
                direction = table->Columns[*sortPick].SortDirection == ImGuiSortDirection_Ascending ? ImGuiSortDirection_Descending
                                                                                                    : ImGuiSortDirection_Ascending;
            }
            ImGui::TableSetColumnSortDirection(*sortPick, direction, false);
        }

        // Sorting, in both views: the list shows the sorted indices as they are, and the tree view
        // walks them, so each parent's children come out in this order under it (#838). Only re-sort
        // when the sort spec changed (SpecsDirty) or when the filtered data changed (filterDirty).
        // Clearing SpecsDirty prevents a redundant O(n log n) sort on every frame when neither the
        // data nor the sort column has changed. m_SortPending covers rows reset to natural order with
        // no sort spec change (#1174).
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            sortSpecs != nullptr && sortSpecs->SpecsCount > 0 && (sortSpecs->SpecsDirty || filterDirty || m_SortPending))
        {
            const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
            const bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);

            // Use ColumnUserID to get ProcessColumn (we set user_id = enum value)
            const std::optional<ProcessColumn> sortColOpt = columnFromUserId(spec.ColumnUserID);
            if (!sortColOpt.has_value())
            {
                sortSpecs->SpecsDirty = true;
                ImGui::EndTable();
                return;
            }

            const ProcessColumn sortCol = *sortColOpt;
            m_SortColumn = sortCol; // Group by orders its groups by this column's totals (#3068)
            m_SortAscending = ascending;

            // Sort m_CachedSortedIndices (a copy of natural order), leaving m_CachedFilteredIndices
            // in natural order for the next sort to start from.
            m_CachedSortedIndices = m_CachedFilteredIndices;
            std::ranges::sort(m_CachedSortedIndices,
                              [&currentSnapshots, sortCol, ascending](size_t a, size_t b)
                              { return ProcessSortUtils::compareByColumn(currentSnapshots[a], currentSnapshots[b], sortCol, ascending); });
            // Pinned processes ahead of the rest, each group in the sort's order (#3061); in tree view
            // that puts pinned roots first, and pinned children first among their siblings.
            ProcessPins::pinnedFirst(m_CachedSortedIndices, currentSnapshots, m_Pins);
            sortSpecs->SpecsDirty = false;
            m_SortPending = false;
            ++m_SortGeneration; // The tree's rows are rebuilt in the new order (#838)
        }

        // Arrow keys, j/k, Page Up/Down, Home/End, g/G and F9, against the rows as they will be drawn
        applyKeyboardInput(currentSnapshots, navCommand, killRequested, selectAllRequested);

        prepareRowMeters(liveSnapshots); // Inline meters (#1528), scaled to the running processes
        // The Settings toggle, read each frame so a change shows at once; and the icons' size (#1800).
        m_IconView.setEnabled(UserConfig::get().settings().showProcessIcons);
        m_IconView.beginFrame();

        // Render process rows - grouped (#3068), tree view or flat list
        if (m_GroupBy != ProcessGrouping::GroupBy::None)
        {
            renderGroupedRows(currentSnapshots);
        }
        else if (m_TreeViewEnabled)
        {
            // Render tree view (tree is rebuilt in onUpdate on refresh timer)
            renderTreeView(currentSnapshots, m_CachedSortedIndices);
        }
        else
        {
            // Render flat list with clipper for performance
            // ImGuiListClipper only renders visible rows, skipping off-screen rows
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_CachedSortedIndices.size()));
            if (m_ScrollSelectedIntoView && m_ScrollTargetRow < m_CachedSortedIndices.size())
            {
                clipper.IncludeItemByIndex(static_cast<int>(m_ScrollTargetRow)); // Drawn, so it can be scrolled to
            }
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    // The indices are rebuilt for every adopted generation, so they always fit the
                    // vector; checked anyway, so a keying mistake can't read past its end (#1394).
                    const std::size_t procIdx = m_CachedSortedIndices[static_cast<size_t>(i)];
                    if (procIdx >= currentSnapshots.size())
                    {
                        continue;
                    }
                    renderProcessRow(currentSnapshots[procIdx],
                                     {.procIdx = procIdx, .depth = 0, .hasChildren = false, .isExpanded = false, .isContext = false});
                }
            }
        }
        m_ScrollSelectedIntoView = false; // Done by the selected row if it was drawn; never carried over
        // A row clicked this frame (#804): applied now the rows are drawn, so a Shift+click's range can
        // read the visible order without rebuilding the tree rows under the loop above.
        applyPendingClick(currentSnapshots);

        // Sync column visibility from ImGui back to our settings
        // This captures changes made via the right-click context menu. Not on a frame the Columns
        // menu changed it: ImGui applies that next frame, so its flags still show the old state.
        // Only once the table has been drawn with our settings is a difference the user's own toggle;
        // before that it is ImGui's restored layout, which must not mark a column chosen (#1210).
        if (!columnsChangedByMenu)
        {
            bool settingsChanged = false;
            int idx = 0;
            for (const ProcessColumn col : allProcessColumns())
            {
                const bool isEnabled = (ImGui::TableGetColumnFlags(idx) & ImGuiTableColumnFlags_IsEnabled) != 0;
                settingsChanged = m_ColumnSettings.adoptTableVisibility(col, isEnabled, m_TableShowsColumnSettings) || settingsChanged;
                ++idx;
            }
            if (settingsChanged)
            {
                UserConfig::get().settings().processColumns = m_ColumnSettings;
            }
        }
        m_TableShowsColumnSettings = true;

        ImGui::EndTable();
    }
}

size_t ProcessesPanel::processCount() const
{
    return m_ProcessModel ? m_ProcessModel->processCount() : 0;
}

bool ProcessesPanel::hasReducedPrivileges() const
{
    return processCapabilities().hasReducedPrivileges;
}

void ProcessesPanel::setGpuModel(const std::shared_ptr<const Domain::GPUModel>& gpuModel)
{
    m_GpuModel = gpuModel;
}

ProcessColumnAvailability::GpuSupport ProcessesPanel::gpuSupport() const
{
    const std::shared_ptr<const Domain::GPUModel> gpuModel = m_GpuModel.lock();
    // An NPU or compute accelerator listed (#1799). Before the model's first publication that is
    // unknown, which counts as listed for the NPU columns' support, as unknown per-process support
    // counts as supported -- but not for NPU %'s default, which waits until one is known (#1870).
    const std::shared_ptr<const Domain::GPUPublication> publication = (gpuModel != nullptr) ? gpuModel->publication() : nullptr;
    // Known to be listed: the machine view's NPU tab's condition, which Process Details' NPU tab follows
    // too (hasAccelerator(), #1869). False while there is no publication yet.
    const bool acceleratorListed = GpuSection::showAcceleratorTab(publication.get());
    const ProcessColumnAvailability::KnownUnsupported known =
        (gpuModel == nullptr) ? ProcessColumnAvailability::KnownUnsupported{}
                              : ProcessColumnAvailability::KnownUnsupported{
                                    .perProcess = gpuModel->perProcessMetricsKnownUnsupported(),
                                    .utilization = gpuModel->perProcessUtilizationKnownUnsupported(),
                                    .npuPerProcess = gpuModel->perProcessNpuMetricsKnownUnsupported(),
                                    .npuUtilization = gpuModel->perProcessNpuUtilizationKnownUnsupported(),
                                };
    return ProcessColumnAvailability::gpuSupport(
        gpuModel != nullptr, known, (publication == nullptr) || acceleratorListed, acceleratorListed);
}

bool ProcessesPanel::hasAccelerator() const
{
    // One condition with the NPU % column's default (#1870) and the machine view's NPU tab (#1799).
    return gpuSupport().acceleratorListed;
}

Platform::ProcessCapabilities ProcessesPanel::processCapabilities() const
{
    if (!m_ProcessModel)
    {
        return Platform::ProcessCapabilities{};
    }
    // The capabilities published with the cached snapshot generation, copied with it under the same
    // lock (#1254), so a per-frame reader takes no lock. Before the first generation is cached, the
    // model's own (current) copy.
    if (m_CachedSnapshotVersion == std::numeric_limits<std::uint64_t>::max())
    {
        return m_ProcessModel->capabilities();
    }
    return m_CachedCapabilities;
}

std::optional<Domain::ProcessSnapshot> ProcessesPanel::findSnapshot(std::int32_t pid) const
{
    if (!m_ProcessModel)
    {
        return std::nullopt;
    }
    // Delegates to ProcessModel::findSnapshot(), which searches under its own lock and
    // copies only the matching entry, instead of copying the entire snapshot vector
    // just to scan it here. Deliberately does NOT use
    // m_CachedRenderSnapshots: that cache is only refreshed while this panel's tab is
    // active (see renderContent()), so it would go stale while e.g. the Process Details
    // tab is showing — this must always reflect the latest published snapshot.
    return m_ProcessModel->findSnapshot(pid);
}

void ProcessesPanel::watchProcess(std::int32_t pid)
{
    if (m_ProcessModel)
    {
        m_ProcessModel->watchProcess(pid);
    }
}

bool ProcessesPanel::watchedSamplesSince(std::uint64_t lastSeenVersion, std::vector<Domain::ProcessSample>& outSamples) const
{
    return m_ProcessModel && m_ProcessModel->watchedSamplesSince(lastSeenVersion, outSamples);
}

void ProcessesPanel::renderProcessRow(const Domain::ProcessSnapshot& proc, const ProcessTreeFlatten::ProcessTreeRow& row)
{
    const std::size_t procIdx = row.procIdx;
    ImGui::TableNextRow();

    // A process that exited a moment ago (#3060): kept as a dimmed row with a steady tint, never
    // selectable or acted on, until ExitingProcessRows::DURATION_SECONDS have passed.
    const bool exited = isExitedRow(procIdx);
    if (exited)
    {
        ImVec4 tint = UI::Theme::get().scheme().processExiting; // the theme's, else its error colour
        tint.w = ExitingProcessRows::TINT_ALPHA;
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(tint));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * ExitingProcessRows::CONTENT_ALPHA);
    }
    // A process stuck in uninterruptible sleep (#3097): a steady tint in the theme's disk-sleep colour,
    // ahead of the new-process tint, since it needs the attention more.
    else if (m_StuckProcesses.stuck(proc.uniqueKey))
    {
        ImVec4 tint = UI::Theme::get().scheme().statusDiskSleep;
        tint.w = StuckProcesses::TINT_ALPHA;
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(tint));
    }
    // A process that started a moment ago (#3060): its row tinted, fading out over a few seconds. The
    // wall clock is read once a frame, not per row.
    else if (UserConfig::get().settings().highlightNewProcesses)
    {
        if (const int frame = ImGui::GetFrameCount(); frame != m_HighlightClockFrame)
        {
            m_HighlightClockFrame = frame;
            m_HighlightNowEpochSeconds = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        }
        if (const std::optional<float> alpha = NewProcessHighlight::alpha(proc.startTimeEpoch, m_HighlightNowEpochSeconds);
            alpha.has_value())
        {
            ImVec4 tint = UI::Theme::get().scheme().processNew; // the theme's, else its success colour
            tint.w = *alpha;
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(tint));
        }
    }

    // Get-or-build the pre-formatted strings for this row. Built lazily here --
    // the first time this specific row is actually rendered after its snapshot
    // data or the current font changes -- rather than eagerly for every process
    // whenever the snapshot version advances, so cost scales with visible rows
    // (bounded by ImGuiListClipper), not total process count (perf-plan #843). A
    // font/size/DPI change alone, with no new data version, must still force a
    // rebuild of this entry, or its cached AlignedCellText widths (measured for
    // the old font) would stay wrong until the next ~1Hz data refresh happens to
    // land. The get-or-build decision itself lives in ProcessRowFormat.h
    // (ImGui-free) so it's directly unit-testable. The font is passed as an
    // identity value, not a pointer: RowFormatCache only ever compares this stamp
    // for equality, so it stores a std::uintptr_t and no address escapes into the
    // long-lived cache map (see #904). The stamp changes on every
    // TextSizeCache::populate(), not with the font's address, which a rebuilt
    // font atlas can reuse (#943).
    const Platform::ProcessCapabilities caps = processCapabilities();
    const RowFormatCache& fmt = ProcessRowFormat::getOrBuildRowFormatCache(
        m_RowFormatCache,
        proc,
        m_CachedSnapshotVersion,
        m_TextSizeCache.stamp,
        ProcessColumnAvailability::rowFormatOptions(
            caps, ProcessColumnAvailability::gpuSupportOfGeneration(m_CachedGpuSupport, m_GpuSupport.accelerator)));

    // Render all columns
    int colIdx = 0;
    for (const ProcessColumn col : allProcessColumns())
    {
        const bool columnVisible = ImGui::TableSetColumnIndex(colIdx);
        ++colIdx;

        // The PID column anchors the row's selectable, so it is entered even when
        // it is not visible. TableSetColumnIndex() returns false for a column
        // scrolled out of view as well as for a hidden one, and skipping PID on
        // that basis skipped the only item that makes the row clickable: with the
        // table scrolled right, rows could not be selected and the selected row
        // lost its highlight (#962). The selectable spans all columns and is drawn
        // and hit-tested against the whole table, not this cell, so it works from a
        // clipped column; PID cannot be hidden (canHide is false), so it is never a
        // disabled one.
        if (col == ProcessColumn::PID)
        {
            if (exited)
            {
                renderExitedPidCell(proc, fmt, columnVisible); // its text only: nothing to select
            }
            else
            {
                renderPidCell(proc, fmt, columnVisible);
            }
            continue;
        }
        if (!columnVisible)
        {
            continue; // Column is hidden or clipped
        }

        // Name carries the tree indent and expander, so it is drawn with the
        // panel's tree state; every other column is a small function looked up by
        // column (#1382).
        if (col == ProcessColumn::Name)
        {
            renderNameCell(proc, fmt, row);
            continue;
        }
        // The meter first, so the cell's text is drawn over it (#1528): O(1) checks per cell, and at
        // most one rectangle or one line (#1612).
        if (m_MetersShown && !exited) // an exited row's last values aren't live: no meter
        {
            ProcessRowMeter::renderCellMeter(m_MeterSettings, col, proc, m_MeterMaxima, m_MeterColors);
            ProcessRowMeter::renderCellSparkline(m_MeterSettings, col, m_CachedRowHistory.get(), procIdx, m_MeterColors);
        }
        CELL_RENDERERS[toIndex(col)].render(proc, fmt, m_TextSizeCache.cells);
    }
    if (exited)
    {
        ImGui::PopStyleVar();
    }
}

void ProcessesPanel::renderExitedPidCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, bool columnVisible)
{
    if (!columnVisible)
    {
        return;
    }
    std::array<char, 16> labelBuf{};
    const auto labelResult = std::to_chars(labelBuf.data(), labelBuf.data() + labelBuf.size() - 1, proc.pid);
    *labelResult.ptr = '\0';
    const std::string_view label(labelBuf.data(), static_cast<std::size_t>(labelResult.ptr - labelBuf.data()));
    renderRightAlignedText(label, cachedTextWidth(label, fmt.pidWidth));
    ImGui::SetItemTooltip("Exited a moment ago");
}

void ProcessesPanel::prepareRowMeters(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    m_MetersShown = m_MeterSettings.anyOn();
    if (m_MeterSettings.sparklineMetrics() == 0)
    {
        m_CachedRowHistory.reset(); // No sparkline: hold no rings (the model publishes none from its next generation)
    }
    if (!m_MetersShown)
    {
        return;
    }
    // The absolute columns are scaled against their largest value in the adopted generation, worked
    // out once when it is adopted, not per frame.
    if (m_MeterMaximaVersion != m_CachedSnapshotVersion)
    {
        m_MeterMaxima.rebuild(snapshots);
        m_MeterMaximaVersion = m_CachedSnapshotVersion;
    }
    const auto& scheme = UI::Theme::get().scheme();
    m_MeterColors = {.low = scheme.processMeterLow, .high = scheme.processMeterHigh};
}

void ProcessesPanel::renderPidCell(const Domain::ProcessSnapshot& proc, const RowFormatCache& fmt, bool columnVisible)
{
    // The PID column: the row's selectable anchor and its right-aligned PID text.
    // The tree indent and expand/collapse control deliberately do NOT live here
    // -- see the comment below and #906.

    // By identity -- PID and start time, exactly (#1503) -- not PID alone: after
    // the selected process exits, a new process given its PID is a different row
    // and must not inherit the highlight. Every selected row is highlighted
    // (#804), an O(1) lookup.
    const ProcessSelection::Identity id = ProcessSelection::identityOf(proc);
    const bool isSelected = m_Selection.contains(id);
    // The primary row of a multi-selection -- the one Process Details shows -- is marked (#1926).
    const ProcessRowSelection::RowState rowState = ProcessRowSelection::rowState(isSelected, id == primaryIdentity(), m_Selection.size());

    // The tree indent and expand/collapse button live in the Name column, not
    // here. This column is a fixed 60px, so indenting it pushed the PID text past
    // the cell's clip rect and silently truncated digits at depth >= 1 -- 589
    // rendered as "5" (see #906). Name is fixed-width too (120px), so it cannot
    // absorb the indent for free -- the indent is clamped against the cell
    // instead, see renderNameCell(). Indenting the name is what comparable
    // process viewers do.

    // Stack-allocated label and selectable ID — avoids heap allocations per
    // visible row per frame
    std::array<char, 16> labelBuf{};
    const auto labelResult = std::to_chars(labelBuf.data(), labelBuf.data() + labelBuf.size() - 1, proc.pid);
    *labelResult.ptr = '\0';
    const std::string_view label(labelBuf.data(), static_cast<std::size_t>(labelResult.ptr - labelBuf.data()));

    // The ImGui ID from the full identity too, so two rows can never share one (#1503):
    // "##pid_select_" + an int32 + '_' + a uint64 fits in 64.
    std::array<char, 64> selectableIdBuf{};
    const auto selRes =
        std::format_to_n(selectableIdBuf.data(), selectableIdBuf.size() - 1, "##pid_select_{}_{}", proc.pid, proc.startTimeTicks);
    *selRes.out = '\0';
    // ImGui fills a hovered row with HeaderHovered even when it is selected, so
    // the row just clicked would show the weaker hover tint until the pointer
    // left it. Keep the selected fill while hovered (#1190).
    if (isSelected)
    {
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_Header));
    }
    const bool clicked =
        ImGui::Selectable(selectableIdBuf.data(), isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
    if (isSelected)
    {
        ImGui::PopStyleColor();
    }
    if (rowState == ProcessRowSelection::RowState::Primary && ImGui::IsItemVisible())
    {
        drawPrimaryRowMarker(UI::Theme::get().scheme().selectionPrimary);
    }
    if (clicked)
    {
        // Plain, Ctrl or Shift click (#804), applied once the rows are drawn (applyPendingClick()).
        const ImGuiIO& io = ImGui::GetIO();
        m_PendingClick = PendingClick{.id = id, .kind = ProcessSelection::clickKindFor(io.KeyCtrl, io.KeyShift)};
    }
    // A keyboard move selected this row (#160): bring it into view below the frozen header.
    if (m_ScrollSelectedIntoView && id == primaryIdentity())
    {
        KeyboardInput::scrollLastItemIntoView();
        m_ScrollSelectedIntoView = false;
    }
    // A right-press selects the row and opens its menu for that same process, in the same
    // frame. ImGui's context-item popup opens on the release instead, by which time the
    // table may have re-sorted and put another process under the pointer (#1365). On a row
    // of a multi-selection the selection is kept, so the menu can act on all of it (#804).
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        if (!isSelected || m_Selection.size() < 2)
        {
            m_Selection.selectOnly(id);
        }
        selectProcess(proc);
        m_RowMenuTarget = proc;
        ImGui::OpenPopup(m_RowMenuPopupId);
    }
    if (!columnVisible)
    {
        return; // Scrolled out of view: the row stays selectable, the PID text is
                // not drawn
    }
    ImGui::SameLine(0.0F, 0.0F);
    // Keep PID text right-aligned in its column in both list and tree modes.
    renderRightAlignedText(label, cachedTextWidth(label, fmt.pidWidth));
}

void ProcessesPanel::renderNameCell(const Domain::ProcessSnapshot& proc,
                                    const RowFormatCache& fmt,
                                    const ProcessTreeFlatten::ProcessTreeRow& row)
{
    const int depth = row.depth;
    const bool hasChildren = row.hasChildren;
    const bool isExpanded = row.isExpanded;
    // Tree depth is expressed here rather than in the PID column, which is a
    // fixed 60px and truncated PIDs once the indent was inside it (#906).
    //
    // Name is fixed-width too (120px by default), so the indent has to come out
    // of the name's own room; it cannot be granted extra width per frame.
    // TableSetupColumn()'s init_width_or_weight is applied to a *resizable*
    // column only while the table is initializing (imgui_tables.cpp:989 gates it
    // on !column_is_resizable), so passing a depth-dependent width on later
    // frames is silently ignored.
    //
    // So the indent yields instead: every level's step shrinks alike until the
    // deepest row in the tree keeps the expander slot plus MIN_NAME_WIDTH_EM of
    // name (#838), and the result is clamped against the cell's actual width
    // as well. Deep trees therefore show a smaller step than usual rather than
    // losing the name or, worse, the expander -- a parent whose expander is
    // pushed out of the cell cannot be collapsed back to a usable width, which
    // is a dead end rather than a cosmetic clip. The step is the same at every
    // level, so a child is always one step right of its parent. The expander
    // slot is the control itself plus the ItemSpacing.x that SameLine() adds
    // after it -- both the button and the leaf Dummy are followed by
    // SameLine(), so the spacing is always paid and must be reserved or the
    // name keeps less room than promised.
    const float expanderWidth = ImGui::GetFrameHeight();
    const float expanderSlotWidth = expanderWidth + ImGui::GetStyle().ItemSpacing.x;
    const float treeEmPx = ImGui::GetFontSize();
    const float reservedForControls = expanderSlotWidth + (ProcessTreeIndent::MIN_NAME_WIDTH_EM * treeEmPx);
    const float cellWidth = ImGui::GetContentRegionAvail().x;
    const float indentPerLevel =
        ProcessTreeIndent::uniformIndentPerLevel(ProcessTreeIndent::INDENT_PER_LEVEL_EM * treeEmPx,
                                                 m_GroupBy != ProcessGrouping::GroupBy::None ? 1 : m_TreeRowsCache.maxDepth(),
                                                 cellWidth,
                                                 reservedForControls);
    // A group's members are indented one level under its header (#3068).
    const bool grouped = m_GroupBy != ProcessGrouping::GroupBy::None;
    const float indentWidth =
        (m_TreeViewEnabled || grouped) ? ProcessTreeIndent::clampedIndent(depth, indentPerLevel, cellWidth, reservedForControls) : 0.0F;
    if (m_TreeViewEnabled && depth > 0)
    {
        drawTreeGuideLines(depth, indentPerLevel, expanderWidth);
    }
    const bool indented = indentWidth > 0.0F;
    if (indented)
    {
        ImGui::Indent(indentWidth);
    }

    if (m_TreeViewEnabled && hasChildren)
    {
        // A caret, pointing down when expanded and right when collapsed (#1209), centred in the
        // same slot a leaf's spacer takes, as tall as the text so the row keeps its height.
        // Stack-allocated button ID: avoids heap allocation per visible row per frame
        std::array<char, 40> buttonIdBuf{};
        const auto btnRes = std::format_to_n(buttonIdBuf.data(), buttonIdBuf.size() - 1, "##tree_btn_{}", proc.uniqueKey);
        *btnRes.out = '\0';
        const ImVec2 slotPos = ImGui::GetCursorScreenPos();
        const float slotWidth = ImGui::GetFrameHeight();
        const bool toggled = ImGui::InvisibleButton(buttonIdBuf.data(), ImVec2(slotWidth, ImGui::GetTextLineHeight()));
        const auto& scheme = UI::Theme::get().scheme();
        const float caretWidth = isExpanded ? m_TextSizeCache.caretDownWidth : m_TextSizeCache.caretRightWidth;
        ImGui::GetWindowDrawList()->AddText(ImVec2(slotPos.x + std::floor((slotWidth - caretWidth) * 0.5F), slotPos.y),
                                            ImGui::GetColorU32(ImGui::IsItemHovered() ? scheme.textPrimary : scheme.textMuted),
                                            isExpanded ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT);
        ImGui::SetItemTooltip(isExpanded ? "Collapse" : "Expand");
        if (toggled)
        {
            // Toggle collapsed state using uniqueKey, in the filter's own set while one is on (#838)
            if (isExpanded)
            {
                activeCollapsedKeys().insert(proc.uniqueKey);
            }
            else
            {
                activeCollapsedKeys().erase(proc.uniqueKey);
            }
            ++m_CollapseGeneration; // The tree's rows are rebuilt next frame (#1138)
        }
        ImGui::SameLine();
    }
    else if (m_TreeViewEnabled)
    {
        // Keep names aligned with their siblings that do have an expander.
        ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), 0.0F));
        ImGui::SameLine();
    }

    // A pinned process's thumbtack (#3061), muted, before its icon.
    if (m_Pins.contains(proc.name))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::get().scheme().textMuted);
        ImGui::TextUnformatted(ICON_FA_THUMBTACK);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Pinned");
        ImGui::SameLine(0.0F, ImGui::GetStyle().ItemInnerSpacing.x);
    }
    // The program's icon, or the generic one, before its name (#1800).
    m_IconView.drawInline(proc);
    // A collapsed parent's child count follows its name (#838), muted. The name keeps room for it, so a
    // long name is cut short with an ellipsis before the badge rather than pushing it out of the cell.
    ProcessTreeBadge::BadgeBuffer badgeBuffer{};
    const std::string_view badge =
        (m_TreeViewEnabled && hasChildren && !isExpanded) ? ProcessTreeBadge::badgeText(row.childCount, badgeBuffer) : std::string_view{};
    const ImGuiStyle& style = ImGui::GetStyle();
    const float badgeWidth = badge.empty() ? 0.0F : ImGui::CalcTextSize(badge.data(), badge.data() + badge.size()).x;
    static_cast<void>(UI::Widgets::cellText(proc.name,
                                            cachedTextWidth(proc.name, fmt.nameWidth),
                                            {.rightAligned = false,
                                             .clippedTooltip = true,
                                             .layoutFullText = false,
                                             .trailingReservePx = badge.empty() ? 0.0F : style.ItemSpacing.x + badgeWidth}));
    if (!badge.empty())
    {
        ImGui::SameLine(0.0F, style.ItemSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::get().scheme().textMuted);
        ImGui::TextUnformatted(badge.data(), badge.data() + badge.size());
        ImGui::PopStyleColor();
        if (ImGui::BeginItemTooltip())
        {
            const std::string tooltip = ProcessTreeBadge::badgeTooltip(row.childCount, !m_CachedSearchTerm.empty());
            ImGui::TextUnformatted(tooltip.data(), tooltip.data() + tooltip.size());
            ImGui::EndTooltip();
        }
    }

    if (indented)
    {
        ImGui::Unindent(indentWidth);
    }
}
ProcessTreeFlatten::ProcessTreeRowsKey ProcessesPanel::treeRowsKey() const noexcept
{
    return {
        .snapshotVersion = m_DisplayVersion, // the rows drawn, exited ones included (#3060)
        .filterGeneration = m_FilterGeneration,
        .sortGeneration = m_SortGeneration,
        .collapseGeneration = m_CollapseGeneration,
        // A filtered tree shows each match's ancestors, dimmed, rather than making it a root (#838).
        .context =
            m_CachedSearchTerm.empty() ? ProcessTreeFlatten::FilterContext::MatchesOnly : ProcessTreeFlatten::FilterContext::WithAncestors,
    };
}

void ProcessesPanel::renderTreeFoldButtons(const std::vector<Domain::ProcessSnapshot>& snapshots, float buttonWidth)
{
    ImGui::SameLine();
    if (ImGui::Button(EXPAND_ALL_BUTTON, ImVec2(buttonWidth, 0.0F)))
    {
        activeCollapsedKeys().clear();
        ++m_CollapseGeneration;
    }
    ImGui::SetItemTooltip("Expand all");
    ImGui::SameLine();
    if (ImGui::Button(COLLAPSE_ALL_BUTTON, ImVec2(buttonWidth, 0.0F)) && ProcessTreeFlatten::collapseAll(snapshots, activeCollapsedKeys()))
    {
        ++m_CollapseGeneration;
    }
    ImGui::SetItemTooltip("Collapse all: only the top-level processes stay in view");
}

void ProcessesPanel::renderTreeView(const std::vector<Domain::ProcessSnapshot>& snapshots, const std::vector<std::size_t>& sortedIndices)
{
    // The expanded, filtered tree flattened into render order -- the roots, and each parent's
    // children, in the table's sort order (#838) -- so ImGuiListClipper below can bound the expensive
    // part, renderProcessRow(), which measures/renders every column, to visible rows only. The rows
    // are rebuilt only when a new snapshot is adopted, the filter result is rebuilt or sorted, or a
    // node is collapsed or expanded, not every frame (#1138); see ProcessTreeFlatten::ProcessTreeRowsCache.
    // Held by reference while rendering: an expander toggled below only advances
    // m_CollapseGeneration, so the rows are rebuilt next frame, never during this loop.
    const std::vector<ProcessTreeFlatten::ProcessTreeRow>& rows =
        m_TreeRowsCache.rows(treeRowsKey(), snapshots, sortedIndices, activeCollapsedKeys());

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    if (m_ScrollSelectedIntoView && m_ScrollTargetRow < rows.size())
    {
        clipper.IncludeItemByIndex(static_cast<int>(m_ScrollTargetRow)); // Drawn, so it can be scrolled to
    }
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const ProcessTreeFlatten::ProcessTreeRow& row = rows[static_cast<std::size_t>(i)];
            // An ancestor shown only to place a match below it is drawn dimmed (#838).
            if (row.isContext)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * CONTEXT_ROW_ALPHA);
            }
            renderProcessRow(snapshots[row.procIdx], row);
            if (row.isContext)
            {
                ImGui::PopStyleVar();
            }
        }
    }
}

// ============================================================================
// Discoverable table affordances (#1209)
// ============================================================================

void ProcessesPanel::renderColumnsMenu()
{
    // The menu shows a request ImGui has not applied yet, so a click is reflected at once.
    const ProcessColumnSettings shown = m_RequestedColumns.value_or(m_ColumnSettings);
    const Platform::ProcessCapabilities caps = processCapabilities();

    // Stays open while columns are ticked on and off, as ImGui's header menu does.
    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
    for (const ProcessColumn col : allProcessColumns())
    {
        const auto info = getColumnInfo(col);
        const bool visible = shown.isVisible(col);
        ImGui::BeginDisabled(!info.canHide); // PID and Name are always shown
        // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - constexpr literals are null-terminated
        if (ImGui::MenuItem(info.menuName.data(), nullptr, visible))
        {
            ProcessColumnSettings next = shown;
            next.requestVisible(col, !visible);
            m_RequestedColumns = next;
        }
        ImGui::EndDisabled();
        if (!ProcessColumnAvailability::isSupported(col, caps, m_GpuSupport))
        {
            const std::string_view note = ProcessColumnAvailability::UNSUPPORTED_COLUMN_NOTE;
            ImGui::SetItemTooltip("%.*s", static_cast<int>(note.size()), note.data());
        }
    }
    ImGui::PopItemFlag();

    ImGui::Separator();
    renderMeterMenu();
    const bool canReset = !ProcessColumnAvailability::hasDefaultColumns(shown, caps, m_GpuSupport) || !m_TableHasDefaultOrder;
    if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT " Reset columns", nullptr, false, canReset))
    {
        // This system's defaults: a column it cannot fill is hidden (#1210)
        m_RequestedColumns = ProcessColumnAvailability::defaultColumns(caps, m_GpuSupport);
        m_ResetColumnOrderRequested = true;
    }
    ImGui::SetItemTooltip("Show the default columns, in their default order");
}

void ProcessesPanel::renderMeterMenu()
{
    if (!ImGui::BeginMenu(ICON_FA_CHART_BAR " Show meter"))
    {
        ImGui::SetItemTooltip("Draw a bar or a sparkline of recent values behind a column's value");
        return;
    }
    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
    // A row per meter column, a radio button per style: every column's choice in view, one click to change.
    constexpr auto STYLES = std::to_array<std::pair<ProcessRowMeter::Style, const char*>>({
        {ProcessRowMeter::Style::Off, "Off"},
        {ProcessRowMeter::Style::Bar, "Bar"},
        {ProcessRowMeter::Style::Sparkline, "Sparkline"},
    });
    if (ImGui::BeginTable("##meter_styles", static_cast<int>(STYLES.size()) + 1, ImGuiTableFlags_SizingFixedFit))
    {
        for (const ProcessColumn col : ProcessRowMeter::METER_COLUMNS)
        {
            const auto info = getColumnInfo(col);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(info.menuName.data(), info.menuName.data() + info.menuName.size());
            if (ProcessRowMeter::scaleOf(col) == ProcessRowMeter::Scale::Absolute)
            {
                ImGui::SetItemTooltip("Scaled against the largest value in the list (a sparkline: in its own history)");
            }
            ImGui::PushID(static_cast<int>(toIndex(col)));
            for (const auto& [style, label] : STYLES)
            {
                ImGui::TableNextColumn();
                bool chosen = false;
                {
                    const UI::Widgets::FrameOutlineScope frameOutline;
                    chosen = ImGui::RadioButton(label, m_MeterSettings.style(col) == style);
                }
                if (chosen)
                {
                    m_MeterSettings.setStyle(col, style);
                    UserConfig::get().settings().processMeters = m_MeterSettings;
                    if (m_ProcessModel)
                    {
                        m_ProcessModel->setRowHistoryMetrics(m_MeterSettings.sparklineMetrics());
                    }
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopItemFlag();
    ImGui::EndMenu();
}

bool ProcessesPanel::applyColumnRequests()
{
    if (m_ResetColumnOrderRequested)
    {
        // ImGui's own "Reset order", applied when the columns are next set up.
        if (ImGuiTable* table = ImGui::GetCurrentTable(); table != nullptr)
        {
            table->IsResetDisplayOrderRequest = true;
        }
        m_ResetColumnOrderRequested = false;
    }

    if (!m_RequestedColumns.has_value())
    {
        return false;
    }
    // Not on a frame ImGui initializes the table (created, or its saved layout loaded): it sets every
    // column's visibility from its flags or that layout when the layout locks, after this, and the
    // request would be lost -- the next frame would then take the difference for the user's own toggle
    // and mark the column chosen (#2736). Kept for the next frame, and still no sync on this one.
    if (const ImGuiTable* table = ImGui::GetCurrentTable(); table != nullptr && (table->IsInitializing || table->IsSettingsRequestLoad))
    {
        return true;
    }
    // TableSetColumnEnabled(false) ignores a column's NoHide flag, so PID and Name are kept shown
    // here whatever the request says (#1209).
    m_RequestedColumns->keepUnhideableColumnsVisible();
    int idx = 0;
    for (const ProcessColumn col : allProcessColumns())
    {
        ImGui::TableSetColumnEnabled(idx, m_RequestedColumns->isVisible(col));
        ++idx;
    }
    m_ColumnSettings = *m_RequestedColumns;
    m_RequestedColumns.reset();
    UserConfig::get().settings().processColumns = m_ColumnSettings;
    return true;
}

void ProcessesPanel::syncNameWidthForViewMode()
{
    if (!m_NameWidthSyncPending)
    {
        return;
    }
    const ImGuiTable* table = ImGui::GetCurrentTable();
    // A table on its first frame isn't laid out yet: ImGui sets MinColumnWidth in its first layout
    // pass, after this, and TableSetColumnWidth asserts against a zero minimum (#1656). Wait a frame.
    if (table == nullptr || table->MinColumnWidth <= 0.0F)
    {
        return;
    }
    m_NameWidthSyncPending = false;
    const auto nameIdx = static_cast<int>(toIndex(ProcessColumn::Name));
    const float currentWidth = table->Columns[nameIdx].WidthGiven;
    if (m_TreeViewEnabled)
    {
        const float treeWidth = ProcessTreeIndent::treeViewNameWidth(currentWidth, ImGui::GetFontSize());
        m_NameWidthBeforeTree = currentWidth;
        m_NameWidthSetForTree = (treeWidth > currentWidth) ? treeWidth : 0.0F;
        if (m_NameWidthSetForTree > 0.0F)
        {
            ImGui::TableSetColumnWidth(nameIdx, treeWidth);
        }
        return;
    }
    if (ProcessTreeIndent::shouldRestoreNameWidth(m_NameWidthSetForTree, currentWidth))
    {
        ImGui::TableSetColumnWidth(nameIdx, m_NameWidthBeforeTree);
    }
    m_NameWidthSetForTree = 0.0F;
}

void ProcessesPanel::setTreeView(bool enabled)
{
    if (enabled == m_TreeViewEnabled)
    {
        return;
    }
    m_TreeViewEnabled = enabled;
    m_NameWidthSyncPending = true;
    if (m_TreeViewEnabled && m_GroupBy != ProcessGrouping::GroupBy::None)
    {
        // Tree and Group by are separate views (#3068): Tree sets the group back to None.
        m_GroupBy = ProcessGrouping::GroupBy::None;
        UserConfig::get().settings().processGroupBy = m_GroupBy;
        m_GroupsBuiltFor.reset();
    }
    if (m_TreeViewEnabled)
    {
        // Keep the layout as it stands now, in list view, so a layout saved later still carries the
        // user's sort if a capture has lost it (#952). Tree view used to drop the sort from ImGui's
        // settings by not being sortable; it sorts too now (#838), so this is only a safety net.
        m_SortBackupLayout = captureTableLayout();
        spdlog::debug("ProcessesPanel: Switched to tree view");
    }
    else
    {
        // ImGui need not mark the sort specs dirty on the way back, so force the sort (#1174).
        m_SortPending = true;
        spdlog::debug("ProcessesPanel: Switched to flat list view");
    }
}

void ProcessesPanel::collectVisibleRows(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                        std::vector<std::size_t>& visible,
                                        std::vector<ProcessTableNavigation::TreeRowShape>* shapes)
{
    // The sorted list, or the flattened tree (from the same cache renderTreeView() draws, so this costs
    // no rebuild).
    visible.clear();
    if (shapes != nullptr)
    {
        shapes->clear();
    }
    if (m_GroupBy != ProcessGrouping::GroupBy::None)
    {
        // The members of the open groups, in the order drawn; the header rows aren't processes (#3068).
        rebuildGroupsIfStale(snapshots);
        visible.reserve(m_GroupedRows.size());
        for (const GroupedRow& row : m_GroupedRows)
        {
            if (!row.header)
            {
                visible.push_back(row.procIdx);
            }
        }
    }
    else if (m_TreeViewEnabled)
    {
        const auto& rows = m_TreeRowsCache.rows(treeRowsKey(), snapshots, m_CachedSortedIndices, activeCollapsedKeys());
        visible.reserve(rows.size());
        if (shapes != nullptr)
        {
            shapes->reserve(rows.size());
        }
        for (const ProcessTreeFlatten::ProcessTreeRow& row : rows)
        {
            visible.push_back(row.procIdx);
            if (shapes != nullptr)
            {
                shapes->push_back({.depth = row.depth, .hasChildren = row.hasChildren, .isExpanded = row.isExpanded});
            }
        }
    }
    else
    {
        visible = m_CachedSortedIndices;
    }
    // The indices fit the adopted generation; checked anyway, as the row loops do (#1394). A dropped
    // index would misalign the tree shapes, so Left/Right are then left to do nothing.
    std::erase_if(visible, [&snapshots](std::size_t idx) { return idx >= snapshots.size(); });
    if (shapes != nullptr && visible.size() != shapes->size())
    {
        shapes->clear();
    }
    // Exited rows (#3060) are drawn but never navigated to, selected or acted on: dropped here, with
    // their tree shapes, so the keyboard and a Shift+click's range skip them.
    if (shapes != nullptr && shapes->size() == visible.size())
    {
        std::size_t kept = 0;
        for (std::size_t i = 0; i < visible.size(); ++i)
        {
            if (!isExitedRow(visible[i]))
            {
                visible[kept] = visible[i];
                (*shapes)[kept] = (*shapes)[i];
                ++kept;
            }
        }
        visible.resize(kept);
        shapes->resize(kept);
    }
    else
    {
        std::erase_if(visible, [this](std::size_t idx) { return isExitedRow(idx); });
    }
}

std::vector<ProcessSelection::Identity> ProcessesPanel::visibleIdentities(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    std::vector<std::size_t> visible;
    collectVisibleRows(snapshots, visible, nullptr);
    std::vector<ProcessSelection::Identity> ids;
    ids.reserve(visible.size());
    for (const std::size_t idx : visible)
    {
        ids.push_back(ProcessSelection::identityOf(snapshots[idx]));
    }
    return ids;
}

void ProcessesPanel::applyPendingClick(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    if (!m_PendingClick.has_value())
    {
        return;
    }
    const PendingClick click = *m_PendingClick;
    m_PendingClick.reset();

    using ProcessSelection::ClickKind;
    const bool isRange = (click.kind == ClickKind::Range || click.kind == ClickKind::AddRange);
    // Only a range reads the visible order: an O(rows) pass on a Shift+click, never on a plain one.
    const std::vector<ProcessSelection::Identity> ids = isRange ? visibleIdentities(snapshots) : std::vector<ProcessSelection::Identity>{};
    m_Selection.click(click.kind, click.id, ids);
    // The clicked row becomes the primary process, unless a Ctrl+click just removed it from the
    // selection: Process Details then keeps showing what it showed.
    if (m_Selection.contains(click.id))
    {
        selectProcess(click.id);
    }
    else if (m_Selection.size() == 1 && !m_Selection.contains(primaryIdentity()))
    {
        // Ctrl+click removed the primary row and left one: that row is the selection now, so it
        // becomes the primary too, or F9 (which acts on the primary) would refuse (#804 review).
        const ProcessSelection::Identity remaining = *m_Selection.selected().begin();
        if (std::ranges::any_of(snapshots,
                                [&remaining](const Domain::ProcessSnapshot& s) { return ProcessSelection::identityOf(s) == remaining; }))
        {
            selectProcess(remaining);
        }
    }
}

void ProcessesPanel::applyKeyboardInput(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                        ProcessTableNavigation::NavCommand command,
                                        bool killRequested,
                                        bool selectAllRequested)
{
    namespace Nav = ProcessTableNavigation;
    const std::optional<NiceStep::Direction> priorityStep = std::exchange(m_PriorityStepRequest, std::nullopt);
    if (command == Nav::NavCommand::None && !killRequested && !selectAllRequested && !priorityStep.has_value() &&
        m_IncrementalSearchStep == IncrementalSearchStep::None)
    {
        return; // The common frame: no key, so the visible order is not even looked at
    }

    // The visible rows in drawn order, as indices into snapshots.
    std::vector<std::size_t> visible;
    std::vector<Nav::TreeRowShape> shapes;
    collectVisibleRows(snapshots, visible, &shapes);

    // Their identities, PID and start time (#1503), parallel to visible.
    std::vector<ProcessSelection::Identity> ids;
    ids.reserve(visible.size());
    for (const std::size_t idx : visible)
    {
        ids.push_back(ProcessSelection::identityOf(snapshots[idx]));
    }

    // Ctrl+A (#804): every row shown -- filtered, and in tree view without collapsed branches.
    if (selectAllRequested)
    {
        m_Selection.selectAll(ids);
    }

    const std::optional<std::size_t> current =
        (m_SelectedPid == -1) ? std::nullopt : Nav::indexOfKey<ProcessSelection::Identity>(ids, primaryIdentity());

    // F9: the row menu's Kill, confirmed in its dialog for the target captured here. Only for a
    // selected row the user can see, only when the platform can kill, and never over a row action
    // already requested or open: a menu action's dialog opens on the next frame, before ImGui knows
    // of it, so the popup stack alone would let F9 replace its action and target (#170).
    // With several rows selected (#804), F9 asks to kill all of them, in the batch confirm; with one,
    // the one highlighted row, when it is visible.
    // A batch priority change (#1484), in its dialog or its confirmation, is a row action too.
    // So is a dump's confirm, progress or result (#807), and a batch still running on its worker (#2142).
    const bool rowActionPending = m_ShowRowActionConfirm || m_RowAction.pending() || m_BatchPriorityDialog.isPending() ||
                                  m_AffinityDialog.isPending() || m_RunTaskDialog.isOpen() || m_FindHandlesDialog.isOpen() ||
                                  m_DumpView.active() || rowActionRunning();
    if (killRequested && m_Selection.size() > 1)
    {
        // Only while at least one selected row is visible: with every selected row filtered out, F9
        // must not open a kill for rows the user cannot see (#804 review).
        if (Detail::isActionAvailable(m_ActionCapabilities, Detail::ProcessAction::Kill) && !rowActionPending &&
            m_Selection.anyVisible(ids))
        {
            requestSelectionAction(Detail::ProcessAction::Kill);
        }
    }
    else if (killRequested && m_Selection.size() == 1)
    {
        // The one highlighted row, found by its identity rather than taken to be the primary: an
        // exited primary is kept while the selection moves on, and Ctrl+A over a filter can select one
        // row without making it primary, yet F9 must act on what is highlighted (#804 review). The
        // identity is PID and start time, compared exactly, so only that process's row can match
        // (#1503).
        const ProcessSelection::Identity selectedId = *m_Selection.selected().begin();
        if (const std::optional<std::size_t> row = Nav::indexOfKey<ProcessSelection::Identity>(ids, selectedId); row.has_value())
        {
            const Domain::ProcessSnapshot& proc = snapshots[visible[*row]];
            if (Detail::killShortcutAllowed(m_ActionCapabilities, selectedId, rowActionPending))
            {
                requestRowAction(Detail::ProcessAction::Kill, proc);
            }
        }
    }
    // F7 / F8 (#3097): the one highlighted row's priority a step higher or lower, at once, as htop does.
    if (priorityStep.has_value() && m_Selection.size() == 1 && m_ActionCapabilities.canSetPriority && !rowActionPending && m_ProcessActions)
    {
        const ProcessSelection::Identity selectedId = *m_Selection.selected().begin();
        if (const std::optional<std::size_t> row = Nav::indexOfKey<ProcessSelection::Identity>(ids, selectedId); row.has_value())
        {
            const Domain::ProcessSnapshot& proc = snapshots[visible[*row]];
            if (const std::optional<std::int32_t> nice = NiceStep::next(proc.nice, proc.priorityClass, *priorityStep); nice.has_value())
            {
                runRowAction(
                    std::format("Setting the priority of {}...", proc.name),
                    [actions = m_ProcessActions.get(), selectedId, nice = *nice] { return actions->setPriority(selectedId, nice); },
                    [name = proc.name, pid = proc.pid, nice = *nice](const Platform::ProcessActionResult& result)
                    { return NiceStep::resultMessage(name, pid, nice, result); },
                    Platform::AuthorizedAction{.kind = Platform::AuthorizedAction::Kind::SetPriority, .target = selectedId, .nice = *nice});
            }
            else
            {
                showBatchOutcome({.message = {.ok = false,
                                              .text = std::format("{} is already at the {} priority",
                                                                  proc.name,
                                                                  *priorityStep == NiceStep::Direction::Raise ? "highest" : "lowest")},
                                  .anySucceeded = false});
            }
        }
    }

    // "/" incremental search (#3097): the row whose name matches, from the selected one, selected alone
    // and scrolled to as a keyboard move is; the Find box says when there is none.
    if (const IncrementalSearchStep step = std::exchange(m_IncrementalSearchStep, IncrementalSearchStep::None);
        step != IncrementalSearchStep::None)
    {
        std::vector<std::string_view> names;
        names.reserve(visible.size());
        for (const std::size_t idx : visible)
        {
            names.emplace_back(snapshots[idx].name);
        }
        const std::optional<std::size_t> match =
            IncrementalSearch::findMatch(names, current, m_IncrementalSearchText, step == IncrementalSearchStep::Next);
        m_IncrementalSearchMissed = !match.has_value() && !m_IncrementalSearchText.empty();
        if (match.has_value())
        {
            m_Selection.selectOnly(ids[*match]);
            if (match != current)
            {
                selectProcess(snapshots[visible[*match]]);
            }
            m_ScrollSelectedIntoView = true;
            m_ScrollTargetRow = *match;
        }
    }

    // No movement key this frame (e.g. a bare F9): nothing below may select or scroll, so a refused F9
    // leaves the table exactly as it was.
    if (command == Nav::NavCommand::None)
    {
        return;
    }

    const auto selectRow = [&](std::size_t row)
    {
        // A move selects that row alone, also out of a multi-selection (#804), and anchors a later
        // Shift+click there.
        m_Selection.selectOnly(ids[row]);
        if (row != current)
        {
            selectProcess(snapshots[visible[row]]);
        }
        m_ScrollSelectedIntoView = true;
        m_ScrollTargetRow = row;
    };

    if (command == Nav::NavCommand::Left || command == Nav::NavCommand::Right)
    {
        // With no visible selection this selects the first row, like every other move.
        const Nav::TreeStep step = Nav::treeStepFrom(shapes, current, command);
        switch (step.kind)
        {
        case Nav::TreeStepKind::Collapse:
            // Tree collapse state stays keyed by uniqueKey: it only decides what is drawn, never
            // which process is selected or acted on.
            activeCollapsedKeys().insert(snapshots[visible[step.index]].uniqueKey);
            ++m_CollapseGeneration; // renderTreeView() rebuilds the rows from it this frame (#1138)
            break;
        case Nav::TreeStepKind::Expand:
            activeCollapsedKeys().erase(snapshots[visible[step.index]].uniqueKey);
            ++m_CollapseGeneration;
            break;
        case Nav::TreeStepKind::Select:
            selectRow(step.index);
            break;
        case Nav::TreeStepKind::None:
            break;
        }
        return;
    }

    // A row is a line of text plus the cell padding above and below it.
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    const std::size_t page = Nav::pageStep(KeyboardInput::tableScrollViewHeight(), rowHeight);
    if (const std::optional<std::size_t> next = Nav::stepSelection(current, command, visible.size(), page); next.has_value())
    {
        selectRow(*next);
    }
}

void ProcessesPanel::renderIncrementalSearchBox()
{
    if (!m_IncrementalSearchOpen)
    {
        return;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0F);
    if (std::exchange(m_IncrementalSearchFocus, false))
    {
        ImGui::SetKeyboardFocusHere();
    }
    bool entered = false;
    {
        const UI::Widgets::FrameOutlineScope frameOutline;
        entered = ImGui::InputTextWithHint(
            "##IncrementalSearch", ICON_FA_MAGNIFYING_GLASS " Find a name", &m_IncrementalSearchText, ImGuiInputTextFlags_EnterReturnsTrue);
    }
    if (ImGui::IsItemEdited())
    {
        m_IncrementalSearchStep = IncrementalSearchStep::Refine;
    }
    if (entered)
    {
        m_IncrementalSearchStep = IncrementalSearchStep::Next;
        m_IncrementalSearchFocus = true; // Enter leaves the box: keep typing or pressing Enter in it
    }
    if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        m_IncrementalSearchOpen = false;
        m_IncrementalSearchMissed = false;
        return;
    }
    ImGui::SetItemTooltip("Jumps to the next process whose name contains this. Enter: the next match. Esc: close.");
    if (m_IncrementalSearchMissed)
    {
        ImGui::SameLine();
        ImGui::TextColored(UI::Theme::get().scheme().textError, "No match");
    }
}

void ProcessesPanel::selectProcess(const Domain::ProcessSnapshot& proc)
{
    selectProcess(ProcessSelection::identityOf(proc));
}

void ProcessesPanel::selectProcess(const ProcessSelection::Identity& id)
{
    m_SelectedPid = id.pid;
    m_SelectedStartTicks = id.startTimeTicks;

    // Emit process selection event for other panels to react, with the exact identity (#1503)
    Core::ProcessSelectedEvent event(id.pid, id.startTimeTicks);
    raiseEvent(event);
}

void ProcessesPanel::renderRowContextMenu(const Domain::ProcessSnapshot& proc)
{
    // Opened on a row of a multi-selection (#804): its actions are for every selected process; Details
    // and Copy stay with the row it was opened on.
    const std::size_t batchCount =
        (m_Selection.size() > 1 && m_Selection.contains(ProcessSelection::identityOf(proc))) ? m_Selection.size() : 0;

    // Only while the menu is open, so formatting here costs nothing on an ordinary frame.
    if (batchCount > 0)
    {
        ImGui::TextDisabled("%zu processes selected", batchCount);
    }
    ImGui::TextDisabled("%s (PID %d)", proc.name.c_str(), proc.pid);
    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_CIRCLE_INFO " Details"))
    {
        selectProcess(proc);
        Core::ShowProcessDetailsEvent event;
        raiseEvent(event);
    }
    // Kept at the top through every sort and filter (#3061): every process of this name.
    const bool pinned = m_Pins.contains(proc.name);
    if (ImGui::MenuItem(pinned ? ICON_FA_THUMBTACK " Unpin###Pin" : ICON_FA_THUMBTACK " Pin to top###Pin",
                        nullptr,
                        false,
                        pinned || proc.name.size() <= ProcessPins::MAX_NAME_BYTES))
    {
        togglePin(proc);
    }
    ImGui::SetItemTooltip(pinned ? "Stop keeping processes named \"%s\" at the top"
                                 : "Keep processes named \"%s\" at the top of the table, through any sort or filter",
                          proc.name.c_str());

    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_COPY " Copy PID"))
    {
        std::array<char, 16> pidText{};
        const auto result = std::to_chars(pidText.data(), pidText.data() + pidText.size() - 1, proc.pid);
        *result.ptr = '\0';
        ImGui::SetClipboardText(pidText.data());
    }
    if (ImGui::MenuItem(ICON_FA_COPY " Copy Name"))
    {
        ImGui::SetClipboardText(proc.name.c_str());
    }
    if (ImGui::MenuItem(ICON_FA_COPY " Copy Command Line", nullptr, false, !proc.command.empty()))
    {
        ImGui::SetClipboardText(proc.command.c_str());
    }

    // No other process action while one runs on its worker (#2142, #2305): its result is still to come.
    const bool actionsIdle = !rowActionRunning();

    // The row's executable in the file manager, and its name in a web search (#3064), as Task Manager's
    // menu has them: for the row the menu was opened on, each on the action worker.
    ImGui::Separator();
    if (m_ActionCapabilities.canOpenFileLocation &&
        ImGui::MenuItem(ICON_FA_FOLDER_OPEN " Open file location###OpenFileLocation", nullptr, false, actionsIdle))
    {
        openRowFileLocation(proc);
    }
    if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS " Search online###SearchOnline", nullptr, false, actionsIdle))
    {
        searchRowOnline(proc);
    }
    ImGui::SetItemTooltip("Searches the web for \"%s process\". Only the name is sent, never its path or command line.", proc.name.c_str());
    // "Kill..." for one process, "Kill 5 processes..." for a selection; the "###" part keeps the item's
    // ID the same whichever it says.
    const auto actionItem = [batchCount, actionsIdle](Detail::ProcessAction action, const char* icon, const char* shortcut)
    {
        const std::string label =
            (batchCount > 0)
                ? std::format("{} {} {} processes...###{}", icon, Detail::actionLabel(action), batchCount, Detail::actionLabel(action))
                : std::format("{} {}...###{}", icon, Detail::actionLabel(action), Detail::actionLabel(action));
        return ImGui::MenuItem(label.c_str(), shortcut, false, actionsIdle);
    };
    const auto request = [this, &proc, batchCount](Detail::ProcessAction action)
    {
        if (batchCount > 0)
        {
            requestSelectionAction(action);
        }
        else
        {
            requestRowAction(action, proc);
        }
    };

    // Only what this platform can do (ProcessActionCapabilities), as in the Actions block.
    const Platform::ProcessActionCapabilities& can = m_ActionCapabilities;
    if (can.canStop || can.canContinue)
    {
        ImGui::Separator();
        if (can.canStop && actionItem(Detail::ProcessAction::Stop, ICON_FA_PAUSE, nullptr))
        {
            request(Detail::ProcessAction::Stop);
        }
        if (can.canContinue && actionItem(Detail::ProcessAction::Resume, ICON_FA_PLAY, nullptr))
        {
            request(Detail::ProcessAction::Resume);
        }
    }
    // The other signals (#3063, Linux): one process, each confirmed with what it usually does.
    if (can.canSendSignals && batchCount == 0 && ImGui::BeginMenu(ICON_FA_BOLT " Send signal###SendSignal", actionsIdle))
    {
        for (const auto [action, hint] : {std::pair{Detail::ProcessAction::SignalHangup, "reload a daemon"},
                                          std::pair{Detail::ProcessAction::SignalInterrupt, "as Ctrl+C"},
                                          std::pair{Detail::ProcessAction::SignalQuit, "exit with a core dump"},
                                          std::pair{Detail::ProcessAction::SignalUser1, "program-defined"},
                                          std::pair{Detail::ProcessAction::SignalUser2, "program-defined"}})
        {
            const std::string label = std::format("{}...###{}", Detail::signalName(action), Detail::signalName(action));
            if (ImGui::MenuItem(label.c_str(), hint))
            {
                requestRowAction(action, proc);
            }
        }
        ImGui::EndMenu();
    }
    // Efficiency mode (#3071), as Task Manager's row menu has it: one process, ticked while it is on.
    // Turning it on is confirmed; turning it off isn't.
    const bool on = Detail::isInEfficiencyMode(proc);
    if (const Detail::EfficiencyModeToggle toggle =
            Detail::efficiencyModeToggle(can, {.pid = proc.pid, .startTimeTicks = proc.startTimeTicks}, on, !actionsIdle, batchCount);
        toggle.state != Detail::EfficiencyModeToggleState::Hidden)
    {
        ImGui::Separator();
        if (ImGui::MenuItem(Detail::EFFICIENCY_MODE_LABEL, nullptr, on, toggle.state == Detail::EfficiencyModeToggleState::Enabled))
        {
            toggleRowEfficiencyMode(proc, on);
        }
        ImGui::SetItemTooltip("%s", toggle.tooltip); // a disabled item still says why
    }
    // One priority for the whole selection (#1484): picked in its own dialog, then confirmed as a batch.
    // A single process's priority stays in Process Details' Actions block.
    if (ProcessBatch::offersBatchPriority(can, batchCount))
    {
        ImGui::Separator();
        const std::string label = std::format("{} Set priority for {} processes...###SetPriority", ICON_FA_GAUGE_HIGH, batchCount);
        if (ImGui::MenuItem(label.c_str(), nullptr, false, actionsIdle))
        {
            requestSelectionPriority();
        }
    }
    // One process's CPUs (#1887), in its own dialog: a selection's processes rarely want the same set.
    if (can.canSetAffinity && batchCount == 0)
    {
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_MICROCHIP " Set affinity...###SetAffinity", nullptr, false, actionsIdle))
        {
            m_AffinityDialog.open({.pid = proc.pid, .startTimeTicks = proc.startTimeTicks},
                                  proc.name,
                                  proc.cpuAffinity,
                                  static_cast<std::size_t>(std::thread::hardware_concurrency()),
                                  // The groups and core kinds to arrange the boxes by (#1881): read once, on the
                                  // sampler thread with the actions, never here (#3182)
                                  m_AffinityLayout);
        }
    }
    // One process's dump file (#807), as Task Manager's row menu offers it; never for a selection.
    if (DumpDetail::offersCreateDump(can, batchCount))
    {
        if (!can.canSetAffinity || batchCount != 0) // else it shares Set affinity...'s group
        {
            ImGui::Separator();
        }
        if (ImGui::MenuItem(DumpDetail::CREATE_DUMP_LABEL, nullptr, false, actionsIdle))
        {
            // The remembered folder is checked on the confirm's worker, never here (#3155).
            m_DumpView.requestInRememberedFolder({.pid = proc.pid, .startTimeTicks = proc.startTimeTicks},
                                                 proc.name,
                                                 DumpDetail::rememberedDumpFolder(),
                                                 m_ProcessActions.get());
        }
        ImGui::SetItemTooltip("%s", DumpDetail::CREATE_DUMP_TOOLTIP);
    }
    if (can.canTerminate || can.canKill)
    {
        // Ending a process can lose its work: in the danger colour, and confirmed in the dialog's
        // danger-coloured button, as in the Actions block (#1273).
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::get().scheme().textError);
        if (can.canTerminate && actionItem(Detail::ProcessAction::Terminate, ICON_FA_XMARK, nullptr))
        {
            request(Detail::ProcessAction::Terminate);
        }
        if (can.canKill && actionItem(Detail::ProcessAction::Kill, ICON_FA_SKULL, "F9"))
        {
            request(Detail::ProcessAction::Kill);
        }
        // The process and everything it started (#3059), as Task Manager's End process tree: one process,
        // with children listed. Terminate where the platform has it, else Kill.
        if (batchCount == 0 && !proc.childrenIndices.empty() &&
            ImGui::MenuItem(ICON_FA_SITEMAP " End process tree###EndProcessTree", nullptr, false, actionsIdle))
        {
            requestTreeAction(can.canTerminate ? Detail::ProcessAction::Terminate : Detail::ProcessAction::Kill, proc);
        }
        ImGui::PopStyleColor();
    }
}

void ProcessesPanel::setGroupBy(ProcessGrouping::GroupBy groupBy)
{
    if (groupBy == m_GroupBy)
    {
        return;
    }
    m_GroupBy = groupBy;
    UserConfig::get().settings().processGroupBy = groupBy; // saved with the other settings
    m_GroupsBuiltFor.reset();
    if (groupBy != ProcessGrouping::GroupBy::None && m_TreeViewEnabled)
    {
        // A group's members are listed flat under its header: back to the List view, as its control does.
        m_TreeViewEnabled = false;
        m_NameWidthSyncPending = true;
        m_SortPending = true;
    }
    spdlog::debug("ProcessesPanel: grouped by {}", ProcessGrouping::groupByLabel(groupBy));
}

void ProcessesPanel::rebuildGroupsIfStale(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    const GroupsKey key{.displayVersion = m_DisplayVersion,
                        .filterGeneration = m_FilterGeneration,
                        .sortGeneration = m_SortGeneration,
                        .collapseGeneration = m_GroupCollapseGeneration,
                        .groupBy = m_GroupBy};
    if (m_GroupsBuiltFor == key)
    {
        return;
    }
    m_GroupsBuiltFor = key;
    // The sorted, filtered rows (pinned first, #3061) gathered by group; an exited row (#3060) is listed
    // under its group but not counted in the totals.
    m_Groups = ProcessGrouping::buildGroups(snapshots, m_CachedSortedIndices, m_GroupBy, m_LiveRowCount, m_SortColumn, m_SortAscending);
    m_GroupedRows.clear();
    m_GroupedRows.reserve(m_CachedSortedIndices.size() + m_Groups.size());
    for (std::size_t g = 0; g < m_Groups.size(); ++g)
    {
        m_GroupedRows.push_back({.header = true, .group = g, .procIdx = 0});
        if (m_CollapsedGroups.contains(m_Groups[g].key))
        {
            continue;
        }
        for (const std::size_t member : m_Groups[g].members)
        {
            m_GroupedRows.push_back({.header = false, .group = g, .procIdx = member});
        }
    }
}

void ProcessesPanel::renderGroupedRows(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    rebuildGroupsIfStale(snapshots);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_GroupedRows.size()));
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const GroupedRow& row = m_GroupedRows[static_cast<std::size_t>(i)];
            if (row.header)
            {
                renderGroupHeaderRow(row.group);
            }
            else if (row.procIdx < snapshots.size())
            {
                renderProcessRow(snapshots[row.procIdx],
                                 {.procIdx = row.procIdx, .depth = 1, .hasChildren = false, .isExpanded = false, .isContext = false});
            }
        }
    }
}

void ProcessesPanel::renderGroupHeaderRow(std::size_t groupIndex)
{
    const ProcessGrouping::Group& group = m_Groups[groupIndex];
    const bool collapsed = m_CollapsedGroups.contains(group.key);
    ImGui::TableNextRow();
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
    int colIdx = 0;
    for (const ProcessColumn col : allProcessColumns())
    {
        const bool columnVisible = ImGui::TableSetColumnIndex(colIdx);
        ++colIdx;
        if (col == ProcessColumn::PID)
        {
            // The row-wide click target, entered even when PID is scrolled out of view, as a process row's is.
            ImGui::PushID(static_cast<int>(groupIndex));
            if (ImGui::Selectable("##group", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            {
                if (collapsed)
                {
                    m_CollapsedGroups.erase(group.key);
                }
                else
                {
                    m_CollapsedGroups.insert(group.key);
                }
                ++m_GroupCollapseGeneration;
            }
            ImGui::SetItemTooltip("%s", collapsed ? "Expand" : "Collapse");
            ImGui::PopID();
            continue;
        }
        if (!columnVisible)
        {
            continue;
        }
        if (col == ProcessColumn::Name)
        {
            const std::string label = std::format("{} {} ({})",
                                                  collapsed ? ICON_FA_CARET_RIGHT : ICON_FA_CARET_DOWN,
                                                  group.key.empty() ? ProcessGrouping::UNGROUPED_LABEL : std::string_view(group.key),
                                                  group.members.size());
            static_cast<void>(
                UI::Widgets::cellText(label,
                                      ImGui::CalcTextSize(label.c_str()).x,
                                      {.rightAligned = false, .clippedTooltip = true, .layoutFullText = false, .trailingReservePx = 0.0F}));
            continue;
        }
        const std::optional<double> total = ProcessGrouping::totalFor(group.totals, col);
        if (!total.has_value())
        {
            continue;
        }
        // Marked with Σ: a rollup must never read as one process's figure (#838).
        std::string text;
        switch (col)
        {
        case ProcessColumn::CpuPercent:
        case ProcessColumn::MemPercent:
        case ProcessColumn::GpuPercent:
        case ProcessColumn::NpuPercent:
            text = UI::Format::formatPercent(*total);
            break;
        case ProcessColumn::IoRead:
        case ProcessColumn::IoWrite:
        case ProcessColumn::NetSent:
        case ProcessColumn::NetReceived:
            text = UI::Format::formatBytesPerSec(*total);
            break;
        case ProcessColumn::Threads:
        case ProcessColumn::Handles:
            text = UI::Format::formatIntLocalized(static_cast<std::int64_t>(*total));
            break;
        case ProcessColumn::CpuTime:
            text = UI::Format::formatDuration(*total);
            break;
        case ProcessColumn::Power:
            text = UI::Format::formatWatts(*total);
            break;
        default:
            text = UI::Format::formatBytes(*total);
            break;
        }
        text.insert(0, "\xCE\xA3 "); // U+03A3
        renderRightAlignedText(text, ImGui::CalcTextSize(text.c_str()).x);
    }
}

void ProcessesPanel::togglePin(const Domain::ProcessSnapshot& proc)
{
    const bool changed = m_Pins.contains(proc.name) ? m_Pins.unpin(proc.name) : m_Pins.pin(proc.name);
    if (!changed)
    {
        showRowActionResult({.ok = false, .text = std::format("Can't pin more than {} processes", ProcessPins::MAX_PINS)}, false);
        return;
    }
    UserConfig::get().settings().pinnedProcesses = m_Pins.names(); // saved with the other settings
    m_PinsChanged = true;
    spdlog::info("ProcessesPanel: {} {}", m_Pins.contains(proc.name) ? "pinned" : "unpinned", proc.name);
}

void ProcessesPanel::openRowFileLocation(const Domain::ProcessSnapshot& proc)
{
    // This row's identity: the platform refuses it if the PID has since been reused (#973).
    const Platform::ProcessTarget target{.pid = proc.pid, .startTimeTicks = proc.startTimeTicks};
    runRowAction(
        std::format("Opening the file location of {}...", proc.name),
        [actions = m_ProcessActions.get(), target] { return actions->openFileLocation(target); },
        [name = proc.name, pid = proc.pid](const Platform::ProcessActionResult& result)
        { return Detail::formatOpenFileLocationResult(name, pid, result); },
        std::nullopt); // the user's own file manager: nothing the helper does
}

void ProcessesPanel::searchRowOnline(const Domain::ProcessSnapshot& proc)
{
    runRowAction(
        std::format("Searching online for {}...", proc.name),
        [url = Detail::searchOnlineUrl(proc.name)]
        {
            return PlatformOpen::openWithSystemHandler(std::string_view{url})
                     ? Platform::ProcessActionResult::ok()
                     : Platform::ProcessActionResult::error("no web browser could be started");
        },
        [name = proc.name](const Platform::ProcessActionResult& result) { return Detail::formatSearchOnlineResult(name, result); },
        std::nullopt);
}

void ProcessesPanel::requestRowAction(Detail::ProcessAction action, const Domain::ProcessSnapshot& proc)
{
    m_RowAction.action = action;
    // This row's identity: the platform refuses the action if the PID has since been reused (#973).
    m_RowAction.targets.assign(
        1,
        ProcessBatch::BatchTarget{.target = {.pid = proc.pid, .startTimeTicks = proc.startTimeTicks}, .name = proc.name, .priority = {}});
    m_RowAction.title = Detail::confirmTitle(action, proc.name, proc.pid);
    m_RowAction.question = Detail::confirmBody(action, proc.name, proc.pid);
    m_ShowRowActionConfirm = true;
}

void ProcessesPanel::toggleRowEfficiencyMode(const Domain::ProcessSnapshot& proc, bool on)
{
    if (!on)
    {
        requestRowAction(Detail::ProcessAction::EfficiencyModeOn, proc); // confirmed, as Task Manager does
        return;
    }
    // Off at once, for this row's identity: the platform refuses it if the PID has since been reused (#973).
    const Platform::ProcessTarget target{.pid = proc.pid, .startTimeTicks = proc.startTimeTicks};
    constexpr Detail::ProcessAction OFF = Detail::ProcessAction::EfficiencyModeOff;
    runRowAction(
        std::format("{} PID {}...", Detail::actionProgressLabel(OFF), target.pid),
        [actions = m_ProcessActions.get(), target] { return Detail::dispatchProcessAction(*actions, OFF, target); },
        [pid = target.pid](const Platform::ProcessActionResult& result) { return Detail::formatActionResultMessage(OFF, pid, result); },
        Detail::authorizedActionFor(OFF, target));
}

void ProcessesPanel::requestSelectionAction(Detail::ProcessAction action)
{
    // The selected processes still listed, each by PID and start time (#973): one that has exited since
    // it was selected is not among them, and one whose PID is reused is refused by the platform.
    std::vector<ProcessBatch::BatchTarget> targets = ProcessBatch::resolveTargets(
        *m_CachedRenderSnapshots, [this](const Platform::ProcessTarget& target) { return m_Selection.contains(target); });
    if (targets.empty())
    {
        return;
    }
    if (targets.size() == 1)
    {
        const ProcessBatch::BatchTarget& only = targets.front();
        m_RowAction.title = Detail::confirmTitle(action, only.name, only.target.pid);
        m_RowAction.question = Detail::confirmBody(action, only.name, only.target.pid);
    }
    else
    {
        m_RowAction.title = ProcessBatch::confirmTitle(action, targets.size());
        m_RowAction.question = ProcessBatch::confirmBody(action, targets, m_OwnPid);
    }
    m_RowAction.action = action;
    m_RowAction.targets = std::move(targets);
    m_ShowRowActionConfirm = true;
}

void ProcessesPanel::requestTreeAction(Detail::ProcessAction action, const Domain::ProcessSnapshot& proc)
{
    std::vector<ProcessBatch::BatchTarget> targets =
        ProcessBatch::resolveTreeTargets(*m_CachedRenderSnapshots, {.pid = proc.pid, .startTimeTicks = proc.startTimeTicks});
    if (targets.size() < 2)
    {
        requestRowAction(action, proc); // its children have exited since the menu opened
        return;
    }
    m_RowAction.title = ProcessBatch::treeConfirmTitle(proc.name, proc.pid, targets.size());
    m_RowAction.question = ProcessBatch::confirmBody(action, targets, m_OwnPid);
    m_RowAction.action = action;
    m_RowAction.targets = std::move(targets);
    m_ShowRowActionConfirm = true;
}

void ProcessesPanel::requestSelectionPriority()
{
    // As requestSelectionAction(): the selected processes still listed, each by PID and start time, so
    // one that has exited is not among them and a reused PID is refused when the priority is set.
    std::vector<ProcessBatch::BatchTarget> targets = ProcessBatch::resolveTargets(
        *m_CachedRenderSnapshots, [this](const Platform::ProcessTarget& target) { return m_Selection.contains(target); });
    if (targets.empty())
    {
        return;
    }
    m_BatchPriorityDialog.open(std::move(targets), m_OwnPid);
}

void ProcessesPanel::renderBatchPriorityDialog()
{
    const std::optional<std::int32_t> nice = m_BatchPriorityDialog.render();
    if (!nice.has_value())
    {
        return;
    }
    // Apply: the dialog listed these processes, so it was the confirmation (#1539). Every one in turn,
    // by identity, TaskSmack itself last, on the batch's worker (#2142); one summary line naming the
    // priority applied (#1484) when it finishes.
    if (!m_ProcessActions)
    {
        showBatchOutcome({.message = {.ok = false, .text = "Process actions unavailable"}, .anySucceeded = false});
        return;
    }
    const std::span<const ProcessBatch::BatchTarget> targets = m_BatchPriorityDialog.targets();
    if (std::optional<ProcessBatch::BatchRunOutcome> failed = m_BatchRun.startPriority(
            *m_ProcessActions, std::vector<ProcessBatch::BatchTarget>(targets.begin(), targets.end()), *nice, m_OwnPid);
        failed.has_value())
    {
        showBatchOutcome(std::move(*failed));
    }
}

void ProcessesPanel::showBatchOutcome(ProcessBatch::BatchRunOutcome outcome)
{
    m_RowRetry.reset();
    m_RowActionResult = std::move(outcome.message);
    m_RowActionResultSeconds = ROW_ACTION_RESULT_SECONDS;
    if (outcome.anySucceeded)
    {
        requestRefresh(); // Show the processes acted on without waiting for the next sample
    }
}

void ProcessesPanel::renderRunTaskDialog()
{
    const std::optional<Platform::RunProgramRequest> request = m_RunTaskDialog.render();
    if (!request.has_value())
    {
        return;
    }
    // Remembered whether or not it starts: a typo is easier to fix from the list than to retype.
    UserConfig::get().settings().recentCommands =
        RunTaskHistory::remember(UserConfig::get().settings().recentCommands, request->commandLine);
    std::string shown = runTaskShortName(request->commandLine);
    runRowAction(
        std::format("Starting {}...", shown),
        [actions = m_ProcessActions.get(), request = *request] { return actions->runProgram(request); },
        [shown = std::move(shown), admin = request->asAdministrator](const Platform::ProcessActionResult& result)
        { return runTaskResultMessage(shown, admin, result); },
        std::nullopt); // its own pkexec prompt when elevated: nothing the helper does
}

void ProcessesPanel::renderFindHandlesDialog(const std::vector<Domain::ProcessSnapshot>& snapshots)
{
    const std::optional<FindHandlesAction> action = m_FindHandlesDialog.render(
        [&snapshots]
        {
            std::vector<Domain::HandleSearch::SearchProcess> processes;
            processes.reserve(snapshots.size());
            for (const Domain::ProcessSnapshot& snapshot : snapshots)
            {
                processes.push_back({.target = ProcessSelection::identityOf(snapshot), .name = snapshot.name});
            }
            return processes;
        });
    if (!action.has_value())
    {
        return;
    }
    m_Selection.selectOnly(action->target);
    selectProcess(action->target);
    if (action->showDetails)
    {
        Core::ShowProcessDetailsEvent event;
        raiseEvent(event);
    }
}

void ProcessesPanel::renderAffinityDialog()
{
    const std::optional<Platform::CpuAffinity> cpus = m_AffinityDialog.render();
    if (!cpus.has_value())
    {
        return;
    }
    const Platform::ProcessTarget target = m_AffinityDialog.target();
    runRowAction(
        std::format("Setting the CPU affinity of PID {}...", target.pid),
        [actions = m_ProcessActions.get(), target, cpus = *cpus] { return actions->setAffinity(target, cpus); },
        [name = m_AffinityDialog.name(), pid = target.pid, cpus = *cpus](const Platform::ProcessActionResult& result)
        { return affinityResultMessage(name, pid, cpus, result); },
        Platform::AuthorizedAction{.kind = Platform::AuthorizedAction::Kind::SetAffinity, .target = target, .cpus = *cpus});
}

template<typename Work>
void ProcessesPanel::runRowAction(std::string pending, Work work, RowResultFormat format, std::optional<Platform::AuthorizedAction> retry)
{
    if (!m_ProcessActions)
    {
        showRowActionResult(format(Platform::ProcessActionResult::error("Process actions unavailable")), false);
        return;
    }
    if (rowActionRunning())
    {
        showRowActionResult(format(Platform::ProcessActionResult::error("Another action is still running")), false);
        return;
    }
    static_cast<void>(m_RowActionRun.start(
        std::move(pending),
        std::move(work),
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks) - the std::function moved into the finish is owned by it, not leaked
        [this, format = std::move(format), retry = std::move(retry)](const Platform::ProcessActionResult& result)
        {
            showRowActionResult(format(result), result.success);
            if (result.permissionDenied && retry.has_value() && m_ProcessActions &&
                m_ProcessActions->actionCapabilities().canAuthorizeInteractively)
            {
                m_RowRetry = RowRetry{.action = *retry, .format = format};
                m_RowActionResultSeconds = Detail::AUTHORIZE_RESULT_SECONDS; // time to read it and decide
            }
        }));
}

void ProcessesPanel::retryRowActionAuthorized()
{
    if (!m_RowRetry.has_value() || !offersAuthenticateAndRetry())
    {
        return;
    }
    RowRetry retry = std::move(*m_RowRetry);
    m_RowRetry.reset();
    runRowAction(
        std::format("Waiting for authorization to act on PID {}...", retry.action.target.pid),
        [actions = m_ProcessActions.get(), action = retry.action](const std::stop_token& stopToken)
        { return actions->runAuthorized(action, stopToken); },
        std::move(retry.format),
        std::nullopt); // a refusal to the administrator isn't offered again
}

void ProcessesPanel::showRowActionResult(Detail::ActionResultMessage message, bool succeeded)
{
    m_RowRetry.reset();
    m_RowActionResult = std::move(message);
    m_RowActionResultSeconds = ROW_ACTION_RESULT_SECONDS;
    if (succeeded)
    {
        requestRefresh(); // Show the process changed or gone without waiting for the next sample
    }
}

void ProcessesPanel::renderRowActionConfirm()
{
    const ProcessActionConfirm::Outcome outcome =
        ProcessActionConfirm::renderText(m_ShowRowActionConfirm, m_RowAction.action, m_RowAction.title, m_RowAction.question);
    if (outcome == ProcessActionConfirm::Outcome::Cancelled)
    {
        m_RowAction = {}; // Nothing is pending any more: F9 may ask again (#170)
        return;
    }
    if (outcome != ProcessActionConfirm::Outcome::Confirmed || m_RowAction.targets.empty())
    {
        return;
    }
    if (m_RowAction.targets.size() > 1 && m_ProcessActions)
    {
        // Every selected process in turn, each checked by the platform as a single action is, then one
        // summary line rather than a message per process (#804). On the batch's worker (#2142): the
        // line comes on a later frame, and the toolbar shows the progress until then.
        std::optional<ProcessBatch::BatchRunOutcome> failed =
            m_BatchRun.startAction(*m_ProcessActions, m_RowAction.action, std::move(m_RowAction.targets), m_OwnPid);
        m_RowAction = {}; // Handed to the worker; nothing is pending any more
        if (failed.has_value())
        {
            showBatchOutcome(std::move(*failed));
        }
        return;
    }
    if (m_RowAction.targets.size() != 1)
    {
        showRowActionResult({.ok = false, .text = "Process actions unavailable"}, false);
        m_RowAction = {};
        return;
    }
    // One process, on the row action worker (#2305): "Killing PID 123..." in the toolbar until its
    // result comes on a later frame.
    const Detail::ProcessAction action = m_RowAction.action;
    const Platform::ProcessTarget target = m_RowAction.targets.front().target;
    m_RowAction = {}; // Acted on once; nothing is pending any more
    runRowAction(
        std::format("{} PID {}...", Detail::actionProgressLabel(action), target.pid),
        [actions = m_ProcessActions.get(), action, target] { return Detail::dispatchProcessAction(*actions, action, target); },
        [action, pid = target.pid](const Platform::ProcessActionResult& result)
        { return Detail::formatActionResultMessage(action, pid, result); },
        Detail::authorizedActionFor(action, target));
}

} // namespace App
